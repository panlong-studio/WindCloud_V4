#include <stdio.h>
#include <string.h>
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "file_cmds.h"
#include "file_transfer.h"
#include "auth.h"
#include "dao_vfs.h"
#include "jwt_utils.h"
#include "path_utils.h"
#include "transfer_ticket.h"

/**
 * @brief  向客户端发送一条普通文本响应
 * @param  client_fd 当前客户端套接字
 * @param  msg 要发送的文本消息
 * @return 无
 */
void send_msg(int client_fd, const char *msg) {
    command_packet_t reply_packet;

    // 服务端的普通响应统一封装成 CMD_TYPE_REPLY。
    // 客户端收到后会按普通文本路径直接打印。
    init_command_packet(&reply_packet, CMD_TYPE_REPLY, msg);

    if (send_command_packet(client_fd, &reply_packet) == -1) {
        LOG_WARN("发送响应失败，客户端fd=%d，消息=%s", client_fd, msg);
        return;
    }
    LOG_DEBUG("响应发送成功，客户端fd=%d，消息=%s", client_fd, msg);
}

/**
 * @brief  从普通命令包中解析命令类型
 * @param  cmd_packet 已接收的普通命令包
 * @return 成功返回命令类型，失败返回 CMD_TYPE_INVALID
 */
static cmd_type_t get_packet_cmd_type(const command_packet_t *cmd_packet) {
    if (cmd_packet == NULL) {
        return CMD_TYPE_INVALID;
    }
    return (cmd_type_t)cmd_packet->cmd_type;
}

/**
 * @brief  根据控制连接当前目录和文件名拼接完整虚拟路径
 * @param  ctx 当前控制连接会话上下文
 * @param  arg 用户输入的文件名
 * @param  full_path 输出参数，用来保存完整虚拟路径
 * @param  size full_path 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int build_transfer_full_path(const ClientContext *ctx, const char *arg,
                                    char *full_path, size_t size) {
    if (ctx == NULL || arg == NULL || full_path == NULL) {
        return -1;
    }

    // 当前第四期仍然把 puts/gets 限制为“当前目录下的一个文件名”。
    // 这样控制连接和传输连接之间的路径语义最直接，也更容易调试。
    if (check_arg_path(arg) != 0 || strchr(arg, '/') != NULL) {
        return -1;
    }

    if (strcmp(ctx->current_path, "/") == 0) {
        if (snprintf(full_path, size, "/%s", arg) >= (int)size) {
            return -1;
        }
    } else {
        if (snprintf(full_path, size, "%s/%s", ctx->current_path, arg) >= (int)size) {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief  在控制连接上为本次上传或下载签发一次性传输票据
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前控制连接会话上下文
 * @param  cmd_type 传输命令类型
 * @param  arg 客户端输入的文件名
 * @return 无
 */
static void handle_transfer_prepare(int client_fd, ClientContext *ctx,
                                    cmd_type_t cmd_type, const char *arg) {
    transfer_ticket_reply_packet_t reply_packet;
    char full_path[FULL_PATH_LEN] = {0};
    char ticket[TRANSFER_TICKET_LEN] = {0};

    if (build_transfer_full_path(ctx, arg, full_path, sizeof(full_path)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "传输预请求失败：文件名不合法", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    if (issue_transfer_ticket(ctx->user_id, cmd_type, full_path, ticket, sizeof(ticket)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "传输预请求失败：签发票据失败", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    init_transfer_ticket_reply_packet(&reply_packet, 1, "传输票据申请成功", ticket);
    send_transfer_ticket_reply_packet(client_fd, &reply_packet);
}

/**
 * @brief  从完整虚拟路径中拆出父目录路径和最后一级文件名
 * @param  full_path 完整虚拟路径，例如 /doc/a.txt
 * @param  parent_path 输出参数，用来保存父目录路径
 * @param  parent_size parent_path 缓冲区大小
 * @param  file_name 输出参数，用来保存最后一级文件名
 * @param  file_size file_name 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int split_full_path(const char *full_path,
                           char *parent_path, size_t parent_size,
                           char *file_name, size_t file_size) {
    const char *last_slash = NULL;
    size_t parent_len = 0;

    if (full_path == NULL || full_path[0] != '/') {
        return -1;
    }

    last_slash = strrchr(full_path, '/');
    if (last_slash == NULL || *(last_slash + 1) == '\0') {
        return -1;
    }

    snprintf(file_name, file_size, "%s", last_slash + 1);
    if (file_name[0] == '\0') {
        return -1;
    }

    if (last_slash == full_path) {
        snprintf(parent_path, parent_size, "%s", "/");
        return 0;
    }

    parent_len = (size_t)(last_slash - full_path);
    if (parent_len + 1 > parent_size) {
        return -1;
    }

    memcpy(parent_path, full_path, parent_len);
    parent_path[parent_len] = '\0';
    return 0;
}

/**
 * @brief  根据完整虚拟路径构造传输连接的会话上下文
 * @param  user_id 用户 id
 * @param  full_path 完整虚拟路径
 * @param  ctx 输出参数，用来保存构造后的上下文
 * @param  file_name 输出参数，用来保存最后一级文件名
 * @return 成功返回 0，失败返回 -1
 */
static int build_transfer_context(int user_id, const char *full_path, ClientContext *ctx, char *file_name) {
    char parent_path[256] = {0};
    int dir_id = 0;
    int node_type = 0;

    if (ctx == NULL || file_name == NULL) {
        return -1;
    }

    if (split_full_path(full_path, parent_path, sizeof(parent_path), file_name, FILE_NAME_LEN) != 0) {
        return -1;
    }

    memset(ctx, 0, sizeof(ClientContext));
    ctx->user_id = user_id;
    snprintf(ctx->current_path, sizeof(ctx->current_path), "%s", parent_path);

    if (strcmp(parent_path, "/") == 0) {
        ctx->current_dir_id = 0;
        return 0;
    }

    if (dao_get_node_by_path(user_id, parent_path, &dir_id, &node_type) != 0) {
        return -1;
    }

    if (node_type != 1) {
        return -1;
    }

    ctx->current_dir_id = dir_id;
    return 0;
}

/**
 * @brief  在上传前先消费一个文件信息包，并返回统一失败响应
 * @param  client_fd 当前客户端套接字
 * @param  file_name 文件名
 * @param  msg 失败提示信息
 * @return 无
 */
static void fail_puts_request(int client_fd, const char *file_name, const char *msg) {
    file_packet_t client_file_packet;
    file_packet_t server_file_packet;

    // 上传协议中，客户端会紧跟着再发一个文件信息包。
    // 这里先把它接掉，后面协议才能保持一致。
    if (recv_file_packet(client_fd, &client_file_packet) <= 0) {
        return;
    }

    init_file_packet(&server_file_packet,
                     CMD_TYPE_PUTS,
                     file_name,
                     client_file_packet.file_size,
                     client_file_packet.file_size,
                     NULL);
    send_file_packet(client_fd, &server_file_packet);
    send_msg(client_fd, msg);
}

/**
 * @brief  在下载开始前返回统一失败文件包
 * @param  client_fd 当前客户端套接字
 * @param  file_name 文件名
 * @return 无
 */
static void fail_gets_request(int client_fd, const char *file_name) {
    file_packet_t server_file_packet;

    init_file_packet(&server_file_packet, CMD_TYPE_GETS, file_name, -1, 0, NULL);
    send_file_packet(client_fd, &server_file_packet);
}

/**
 * @brief  处理一条控制连接上的单个命令
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前控制连接会话上下文
 * @param  cmd_packet 已接收的命令包
 * @return 无
 */
void dispatch_control_command(int client_fd, ClientContext *ctx, const command_packet_t *cmd_packet) {
    cmd_type_t cmd_type = get_packet_cmd_type(cmd_packet);
    char arg_buf[CMD_DATA_LEN] = {0};

    LOG_DEBUG("收到控制命令，客户端fd=%d，命令类型=%d，数据=%s",
              client_fd,
              cmd_type,
              cmd_packet->data);

    // 某些业务函数内部会修改参数字符串。
    // 这里先复制一份可写副本，避免直接改原始命令包。
    snprintf(arg_buf, sizeof(arg_buf), "%s", cmd_packet->data);

    // 没登录时，除了登录和注册，别的命令都不允许执行。
    if(ctx->user_id == -1 && cmd_type != CMD_TYPE_LOGIN && cmd_type != CMD_TYPE_REGISTER) {
        LOG_WARN("未登录用户尝试执行命令，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
        send_msg(client_fd, "请先登录!");
        return;
    }

    // 短命令由控制连接直接处理。
    // 上传和下载要求走独立传输连接。
    switch (cmd_type) {
        case CMD_TYPE_LOGIN: {
            int old_user_id = ctx->user_id;
            handle_login(client_fd, arg_buf, &ctx->user_id);

            if (old_user_id == -1 && ctx->user_id != -1) {
                LOG_INFO("用户登录成功，客户端fd=%d，用户id=%d", client_fd, ctx->user_id);
                strcpy(ctx->current_path, "/");
                ctx->current_dir_id = 0;
            }
            break;
        }

        case CMD_TYPE_REGISTER: {
            int temp_new_id=-1;
            handle_register(client_fd, arg_buf, &temp_new_id);
            if(temp_new_id!=-1){
                LOG_INFO("新用户注册成功，客户端fd=%d，新用户id=%d", client_fd, temp_new_id);
            } else {
                LOG_WARN("用户注册失败，客户端fd=%d", client_fd);
            }
            break;
        }

        case CMD_TYPE_PWD:
            handle_pwd(client_fd, ctx);
            break;
        case CMD_TYPE_CD:
            handle_cd(client_fd, ctx, arg_buf);
            break;
        case CMD_TYPE_LS:
            handle_ls(client_fd, ctx);
            break;
        case CMD_TYPE_TOUCH:
            handle_touch(client_fd, ctx, arg_buf);
            break;
        case CMD_TYPE_RM:
            handle_rm(client_fd, ctx, arg_buf);
            break;
        case CMD_TYPE_MKDIR:
            handle_mkdir(client_fd, ctx, arg_buf);
            break;
        case CMD_TYPE_RMDIR:
            handle_rmdir(client_fd, ctx, arg_buf);
            break;
        case CMD_TYPE_GETS:
        case CMD_TYPE_PUTS:
            handle_transfer_prepare(client_fd, ctx, cmd_type, arg_buf);
            break;
        default:
            LOG_WARN("命令类型无效，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
            send_msg(client_fd, "指令错误!");
            break;
    }
}

/**
 * @brief  处理一条传输连接上的完整请求
 * @param  client_fd 当前客户端套接字
 * @return 无
 */
void handle_transfer_request(int client_fd) {
    transfer_auth_packet_t auth_packet;
    ClientContext ctx;
    char full_path[FULL_PATH_LEN] = {0};
    char file_name[FILE_NAME_LEN] = {0};
    int user_id = -1;
    cmd_type_t cmd_type = CMD_TYPE_INVALID;

    // 传输线程进入后，先收一个传输认证包。
    // 这里面只带一次性传输票据。
    if (recv_transfer_auth_packet(client_fd, &auth_packet) <= 0) {
        LOG_WARN("接收传输认证信息失败，客户端fd=%d", client_fd);
        return;
    }

    // 先校验票据，再立即消费。
    // 同一张票据只允许成功使用一次，防止被重复复用。
    if (verify_and_consume_transfer_ticket(auth_packet.ticket,
                                           &user_id,
                                           &cmd_type,
                                           full_path,
                                           sizeof(full_path)) != 0) {
        LOG_WARN("传输连接票据校验失败，客户端fd=%d", client_fd);
        return;
    }

    if (build_transfer_context(user_id, full_path, &ctx, file_name) != 0) {
        LOG_WARN("构造传输上下文失败，客户端fd=%d，用户id=%d，路径=%s",
                 client_fd,
                 user_id,
                 full_path);

        if (cmd_type == CMD_TYPE_GETS) {
            fail_gets_request(client_fd, file_name);
        } else if (cmd_type == CMD_TYPE_PUTS) {
            fail_puts_request(client_fd, file_name, "上传失败：目标目录不存在");
        }
        return;
    }

    // 传输连接只允许处理上传或下载。
    if (cmd_type == CMD_TYPE_GETS) {
        handle_gets(client_fd, &ctx, file_name);
        return;
    }

    if (cmd_type == CMD_TYPE_PUTS) {
        handle_puts(client_fd, &ctx, file_name);
        return;
    }

    LOG_WARN("传输连接命令类型无效，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
}

/**
 * @brief  处理一个客户端连接上的完整请求生命周期
 * @param  client_fd 当前客户端套接字
 * @return 无
 */
void handle_request(int client_fd) {
    ClientContext ctx;

    // 这个函数保留原来的“循环收控制命令”逻辑，
    // 以便旧流程仍然可复用。
    memset(&ctx, 0, sizeof(ctx));
    ctx.user_id = -1;
    strcpy(ctx.current_path, "/");
    ctx.current_dir_id = 0;

    while (1) {
        command_packet_t cmd_packet;

        if (recv_command_packet(client_fd, &cmd_packet) <= 0) {
            LOG_INFO("客户端连接断开，客户端fd=%d，当前路径=%s", client_fd, ctx.current_path);
            break;
        }

        dispatch_control_command(client_fd, &ctx, &cmd_packet);
    }
}
