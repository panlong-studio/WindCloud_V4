#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "file_cmds.h"
#include "file_transfer.h"
#include "auth.h"
#include "dao_file.h"
#include "dao_file_source.h"
#include "dao_vfs.h"
#include "jwt_utils.h"
#include "path_utils.h"
#include "server_identity.h"
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
 * @brief  从控制命令参数中解析分片下载票据申请内容
 * @param  request_str 控制连接上传来的原始字符串
 * @param  file_name 输出参数，用来保存文件名
 * @param  file_name_size file_name 缓冲区大小
 * @param  range_start 输出参数，用来保存起始位置
 * @param  range_end 输出参数，用来保存结束位置
 * @param  source_ip 输出参数，用来保存数据源服务器 IP
 * @param  source_ip_size source_ip 缓冲区大小
 * @param  source_port 输出参数，用来保存数据源服务器端口
 * @param  source_port_size source_port 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int parse_gets_range_ticket_request(const char *request_str,
                                           char *file_name, size_t file_name_size,
                                           off_t *range_start, off_t *range_end,
                                           char *source_ip, size_t source_ip_size,
                                           char *source_port, size_t source_port_size) {
    char buf[CMD_DATA_LEN] = {0};
    char *saveptr = NULL;
    char *token = NULL;

    if (request_str == NULL || file_name == NULL || file_name_size == 0 ||
        range_start == NULL || range_end == NULL ||
        source_ip == NULL || source_ip_size == 0 ||
        source_port == NULL || source_port_size == 0) {
        return -1;
    }

    if (snprintf(buf, sizeof(buf), "%s", request_str) >= (int)sizeof(buf)) {
        return -1;
    }

    // 控制连接上的区间票据申请，约定格式为：
    // 文件名|起始位置|结束位置|数据源IP|数据源端口
    token = strtok_r(buf, "|", &saveptr);
    if (token == NULL || snprintf(file_name, file_name_size, "%s", token) >= (int)file_name_size) {
        return -1;
    }

    token = strtok_r(NULL, "|", &saveptr);
    if (token == NULL) {
        return -1;
    }
    // 第二段和第三段是当前分片负责的字节区间。
    *range_start = (off_t)atoll(token);

    token = strtok_r(NULL, "|", &saveptr);
    if (token == NULL) {
        return -1;
    }
    *range_end = (off_t)atoll(token);

    token = strtok_r(NULL, "|", &saveptr);
    if (token == NULL || snprintf(source_ip, source_ip_size, "%s", token) >= (int)source_ip_size) {
        return -1;
    }

    token = strtok_r(NULL, "|", &saveptr);
    if (token == NULL || snprintf(source_port, source_port_size, "%s", token) >= (int)source_port_size) {
        return -1;
    }

    // 最基本的区间合法性检查。
    // 这里只检查格式层面的合理性，真正是否越界还要等后面结合文件大小再判断。
    if (*range_start < 0 || *range_end < *range_start) {
        return -1;
    }

    return 0;
}

/**
 * @brief  查询某个真实文件当前可用的数据源列表，必要时回退到当前服务器
 * @param  file_id files 表中的真实文件 id
 * @param  sources 输出参数，用来保存数据源列表
 * @param  out_count 输出参数，返回实际数量
 * @return 成功返回 0，失败返回 -1
 */
static int load_available_sources_for_file(int file_id, source_server_t *sources, int *out_count) {
    char current_ip[64] = {0};
    char current_port[16] = {0};

    if (file_id <= 0 || sources == NULL || out_count == NULL) {
        return -1;
    }

    if (dao_file_source_list(file_id, sources, MAX_SOURCE_SERVER_COUNT, out_count) == 0 &&
        *out_count > 0) {
        return 0;
    }

    // 当前项目里，很多旧文件是在第五期之前上传的。
    // 它们可能还没有 file_sources 记录，所以这里给当前服务器加一个最简单的回退来源。
    if (get_current_server_address(current_ip, sizeof(current_ip), current_port, sizeof(current_port)) != 0) {
        return -1;
    }

    snprintf(sources[0].ip, sizeof(sources[0].ip), "%s", current_ip);
    snprintf(sources[0].port, sizeof(sources[0].port), "%s", current_port);
    *out_count = 1;
    return 0;
}

/**
 * @brief  判断客户端请求的数据源是否在当前可用列表里
 * @param  sources 当前可用数据源列表
 * @param  source_count 当前可用数据源数量
 * @param  source_ip 客户端请求的数据源 IP
 * @param  source_port 客户端请求的数据源端口
 * @return 命中返回 1，否则返回 0
 */
static int is_requested_source_allowed(const source_server_t *sources, int source_count,
                                       const char *source_ip, const char *source_port) {
    int i = 0;

    if (sources == NULL || source_ip == NULL || source_port == NULL) {
        return 0;
    }

    for (i = 0; i < source_count; ++i) {
        if (strcmp(sources[i].ip, source_ip) == 0 &&
            strcmp(sources[i].port, source_port) == 0) {
            return 1;
        }
    }

    return 0;
}

/**
 * @brief  构造一份多点下载方案并返回给客户端
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前控制连接会话上下文
 * @param  arg 用户输入的文件名
 * @return 无
 */
static void handle_multi_gets_plan(int client_fd, ClientContext *ctx, const char *arg) {
    multi_gets_plan_packet_t reply_packet;
    char full_path[FULL_PATH_LEN] = {0};
    char sha256sum[65] = {0};
    source_server_t sources[MAX_SOURCE_SERVER_COUNT];
    int node_id = 0;
    int file_id = 0;
    int source_count = 0;
    off_t file_size = 0;
    int i = 0;

    memset(sources, 0, sizeof(sources));

    // 第一步：把控制连接当前目录和文件名拼成完整虚拟路径。
    if (build_transfer_full_path(ctx, arg, full_path, sizeof(full_path)) != 0) {
        init_multi_gets_plan_packet(&reply_packet, 0, arg, -1, NULL, 0, "多点下载方案申请失败：文件名不合法");
        send_multi_gets_plan_packet(client_fd, &reply_packet);
        return;
    }

    // 第二步：从 paths 表里找到这条逻辑路径对应的真实 file_id。
    if (dao_get_file_info_by_path(ctx->user_id, full_path, &node_id, &file_id) != 0) {
        init_multi_gets_plan_packet(&reply_packet, 0, arg, -1, NULL, 0, "多点下载方案申请失败：文件不存在");
        send_multi_gets_plan_packet(client_fd, &reply_packet);
        return;
    }

    // 第三步：从 files 表里拿到总大小和 hash，客户端后面要用它们来切分分片和做最终校验。
    if (dao_file_get_info_by_id(file_id, sha256sum, &file_size) != 0) {
        init_multi_gets_plan_packet(&reply_packet, 0, arg, -1, NULL, 0, "多点下载方案申请失败：查询文件信息失败");
        send_multi_gets_plan_packet(client_fd, &reply_packet);
        return;
    }

    // 第四步：查询当前有哪些服务器可以提供这份真实文件。
    if (load_available_sources_for_file(file_id, sources, &source_count) != 0) {
        init_multi_gets_plan_packet(&reply_packet, 0, arg, -1, NULL, 0, "多点下载方案申请失败：没有可用数据源");
        send_multi_gets_plan_packet(client_fd, &reply_packet);
        return;
    }

    // 第五步：把文件总大小、hash、数据源列表一次性返回给客户端。
    // 客户端拿到这份方案后，才会在本地计算每个分片该找哪个服务器下载。
    init_multi_gets_plan_packet(&reply_packet, 1, arg, file_size, sha256sum, source_count, "多点下载方案申请成功");
    for (i = 0; i < source_count; ++i) {
        snprintf(reply_packet.sources[i].ip, sizeof(reply_packet.sources[i].ip), "%s", sources[i].ip);
        snprintf(reply_packet.sources[i].port, sizeof(reply_packet.sources[i].port), "%s", sources[i].port);
    }

    send_multi_gets_plan_packet(client_fd, &reply_packet);
}

/**
 * @brief  在控制连接上为某个下载分片签发区间票据
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前控制连接会话上下文
 * @param  arg 客户端上传来的原始参数串
 * @return 无
 */
static void handle_gets_range_ticket_prepare(int client_fd, ClientContext *ctx, const char *arg) {
    transfer_ticket_reply_packet_t reply_packet;
    char file_name[FILE_NAME_LEN] = {0};
    char full_path[FULL_PATH_LEN] = {0};
    char ticket[TRANSFER_TICKET_LEN] = {0};
    char source_ip[64] = {0};
    char source_port[16] = {0};
    source_server_t sources[MAX_SOURCE_SERVER_COUNT];
    int node_id = 0;
    int file_id = 0;
    int source_count = 0;
    off_t range_start = 0;
    off_t range_end = 0;

    // 第一步：先把控制连接上传来的区间申请字符串拆开。
    if (parse_gets_range_ticket_request(arg,
                                        file_name,
                                        sizeof(file_name),
                                        &range_start,
                                        &range_end,
                                        source_ip,
                                        sizeof(source_ip),
                                        source_port,
                                        sizeof(source_port)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "下载分片票据申请失败：参数不合法", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    // 第二步：把文件名补成完整虚拟路径。
    if (build_transfer_full_path(ctx, file_name, full_path, sizeof(full_path)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "下载分片票据申请失败：文件名不合法", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    // 第三步：确认这个用户当前确实能访问这个逻辑文件。
    if (dao_get_file_info_by_path(ctx->user_id, full_path, &node_id, &file_id) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "下载分片票据申请失败：文件不存在", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    // 第四步：确认客户端请求的数据源，真的在服务端登记的可用列表里。
    if (load_available_sources_for_file(file_id, sources, &source_count) != 0 ||
        !is_requested_source_allowed(sources, source_count, source_ip, source_port)) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "下载分片票据申请失败：数据源服务器不可用", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    // 第五步：签发一次性区间票据。
    // 票据里同时绑定：
    // 1. 用户
    // 2. 命令类型
    // 3. 完整路径
    // 4. 允许访问的字节区间
    // 5. 允许连接的数据源服务器
    if (issue_transfer_ticket(ctx->user_id,
                              CMD_TYPE_GETS,
                              full_path,
                              range_start,
                              range_end,
                              source_ip,
                              source_port,
                              ticket,
                              sizeof(ticket)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "下载分片票据申请失败：签发票据失败", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    init_transfer_ticket_reply_packet(&reply_packet, 1, "下载分片票据申请成功", ticket);
    send_transfer_ticket_reply_packet(client_fd, &reply_packet);
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
    char current_ip[64] = {0};
    char current_port[16] = {0};

    // puts 的单次传输仍然是“完整文件一次传输”，因此这里不需要区间信息。
    if (build_transfer_full_path(ctx, arg, full_path, sizeof(full_path)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "传输预请求失败：文件名不合法", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    if (get_current_server_address(current_ip, sizeof(current_ip), current_port, sizeof(current_port)) != 0) {
        init_transfer_ticket_reply_packet(&reply_packet, 0, "传输预请求失败：读取当前服务器地址失败", NULL);
        send_transfer_ticket_reply_packet(client_fd, &reply_packet);
        return;
    }

    // 这里签发的是普通传输票据：
    // 区间固定为 0 到 -1，表示不限制区间，由具体上传逻辑自行处理。
    if (issue_transfer_ticket(ctx->user_id,
                              cmd_type,
                              full_path,
                              0,
                              -1,
                              current_ip,
                              current_port,
                              ticket,
                              sizeof(ticket)) != 0) {
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

    // 第一步：先把完整路径拆成“父目录 + 文件名”。
    if (split_full_path(full_path, parent_path, sizeof(parent_path), file_name, FILE_NAME_LEN) != 0) {
        return -1;
    }

    memset(ctx, 0, sizeof(ClientContext));
    ctx->user_id = user_id;
    snprintf(ctx->current_path, sizeof(ctx->current_path), "%s", parent_path);

    // 根目录是一个特例，它没有实际的 paths 目录节点 id，项目里一直用 0 表示。
    if (strcmp(parent_path, "/") == 0) {
        ctx->current_dir_id = 0;
        return 0;
    }

    // 非根目录时，要再去数据库里把父目录的 node_id 查出来。
    // 这样后面的 puts/gets 代码才能继续沿用原来按 current_dir_id 工作的实现。
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
        // 第五期后，控制连接上的 gets 不再直接进入文件传输。
        // 它的职责改成“返回多点下载方案”。
        case CMD_TYPE_GETS:
            handle_multi_gets_plan(client_fd, ctx, arg_buf);
            break;
        // 下载分片票据申请也走控制连接。
        case CMD_TYPE_GETS_RANGE:
            handle_gets_range_ticket_prepare(client_fd, ctx, arg_buf);
            break;
        // puts 控制连接只负责签发传输票据。
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
    char source_ip[64] = {0};
    char source_port[16] = {0};
    char current_ip[64] = {0};
    char current_port[16] = {0};
    int user_id = -1;
    cmd_type_t cmd_type = CMD_TYPE_INVALID;
    off_t range_start = 0;
    off_t range_end = -1;

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
                                           sizeof(full_path),
                                           &range_start,
                                           &range_end,
                                           source_ip,
                                           sizeof(source_ip),
                                           source_port,
                                           sizeof(source_port)) != 0) {
        LOG_WARN("传输连接票据校验失败，客户端fd=%d", client_fd);
        return;
    }

    // 第五期后，票据不只绑定用户和路径，还绑定允许连接的数据源服务器。
    // 这样客户端就不能拿着同一张分片票据去连别的服务器。
    if (get_current_server_address(current_ip, sizeof(current_ip), current_port, sizeof(current_port)) != 0) {
        LOG_WARN("读取当前服务器地址失败，客户端fd=%d", client_fd);
        return;
    }

    if (strcmp(source_ip, current_ip) != 0 || strcmp(source_port, current_port) != 0) {
        LOG_WARN("传输票据绑定的数据源服务器不匹配，客户端fd=%d，票据数据源=%s:%s，当前服务器=%s:%s",
                 client_fd,
                 source_ip,
                 source_port,
                 current_ip,
                 current_port);
        return;
    }

    // 票据验证通过后，还要把“用户 + 完整路径”重新还原成旧版上传下载代码认识的 ClientContext。
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
    // 下载票据可能是“整文件下载”，也可能是“区间下载”。
    // 第五期多点下载走的是区间分支。
    if (cmd_type == CMD_TYPE_GETS) {
        if (range_end >= range_start && range_end >= 0) {
            handle_gets_range(client_fd, &ctx, file_name, range_start, range_end);
        } else {
            handle_gets(client_fd, &ctx, file_name);
        }
        return;
    }

    // 上传仍然只支持整文件传输。
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
