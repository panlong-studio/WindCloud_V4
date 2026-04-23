#include <stdio.h>
#include <string.h>
#include <time.h>
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "file_cmds.h"
#include "file_transfer.h"
#include "auth.h"
#include "jwt.h"
#include "dao_vfs.h"

#define JWT_EXPIRE_SECONDS 3600

/**
 * @brief  向客户端发送一条普通文本响应
 * @param  client_fd 当前客户端套接字
 * @param  msg 要发送的文本消息
 * @return 无
 */
void send_msg(int client_fd, const char *msg) {
    command_packet_t reply_packet;

    // 第一步：把文本消息封装成统一的普通响应包。
    init_command_packet(&reply_packet, CMD_TYPE_REPLY, msg);

    // 第二步：把响应包发送给客户端。
    if (send_command_packet(client_fd, &reply_packet) == -1) {
        LOG_WARN("发送响应失败，客户端fd=%d，消息=%s", client_fd, msg);
        return;
    }

    LOG_DEBUG("响应发送成功，客户端fd=%d，消息=%s", client_fd, msg);
}

/**
 * @brief  从登录参数里提取用户名
 * @param  login_data 登录参数字符串，格式为 用户名/密码
 * @param  user_name 输出参数，用来保存用户名
 * @param  user_name_size user_name 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int parse_login_user_name(const char *login_data, char *user_name, size_t user_name_size) {
    char tmp[CMD_DATA_LEN] = {0};
    char *name_ptr = NULL;

    // 第一步：先校验输入参数。
    if (login_data == NULL || user_name == NULL || user_name_size == 0) {
        return -1;
    }

    // 第二步：把原始登录字符串复制到临时缓冲区。
    // 后续需要使用 strtok 做分隔，因此不能直接修改调用方传入的内容。
    strncpy(tmp, login_data, sizeof(tmp) - 1);

    // 第三步：按“用户名/密码”格式提取用户名。
    name_ptr = strtok(tmp, "/");
    if (name_ptr == NULL) {
        return -1;
    }

    // 第四步：把用户名复制给调用方。
    strncpy(user_name, name_ptr, user_name_size - 1);
    user_name[user_name_size - 1] = '\0';
    return 0;
}

/**
 * @brief  根据当前逻辑路径恢复 current_dir_id
 * @param  user_id 用户 id
 * @param  current_path 当前逻辑路径
 * @param  current_dir_id 输出参数，用来保存目录节点 id
 * @return 成功返回 0，失败返回 -1
 */
static int restore_current_dir_id(int user_id, const char *current_path, int *current_dir_id) {
    int node_id = 0;
    int node_type = 0;

    // 第一步：校验输入参数。
    if (current_path == NULL || current_dir_id == NULL) {
        return -1;
    }

    // 第二步：根目录在当前项目中约定为 current_dir_id = 0。
    if (strcmp(current_path, "/") == 0) {
        *current_dir_id = 0;
        return 0;
    }

    // 第三步：对于非根目录路径，到 paths 表中查询目录节点。
    if (dao_get_node_by_path(user_id, current_path, &node_id, &node_type) != 0) {
        return -1;
    }

    // 第四步：只有目录节点才能作为 current_dir_id。
    if (node_type != 1) {
        return -1;
    }

    // 第五步：回填目录节点 id。
    *current_dir_id = node_id;
    return 0;
}

/**
 * @brief  在主线程中处理一个普通命令
 * @param  client_fd 当前客户端套接字
 * @param  state 当前连接状态
 * @param  cmd_packet 已接收的普通命令包
 * @return 成功返回 0，失败返回 -1
 */
int session_handle_main_command(int client_fd, ServerConnState *state, command_packet_t *cmd_packet) {
    ClientContext ctx;
    cmd_type_t cmd_type;

    // 第一步：校验输入参数。
    if (state == NULL || cmd_packet == NULL) {
        return -1;
    }

    // 第二步：把当前连接状态转换成业务层使用的 ClientContext。
    if (conn_state_to_client_ctx(state, &ctx) != 0) {
        return -1;
    }

    // 第三步：读取本次命令类型，供后续分发使用。
    cmd_type = (cmd_type_t)cmd_packet->cmd_type;
    LOG_DEBUG("主线程处理命令，客户端fd=%d，命令类型=%d，参数=%s", client_fd, cmd_type, cmd_packet->data);

    // 第四步：未登录状态只允许 login 和 register。
    if (ctx.user_id == -1 && cmd_type != CMD_TYPE_LOGIN && cmd_type != CMD_TYPE_REGISTER) {
        send_msg(client_fd, "请先登录!");
        return 0;
    }

    // 第五步：按命令类型分发到对应业务处理函数。
    switch (cmd_type) {
        case CMD_TYPE_LOGIN: {
            int old_user_id = ctx.user_id;
            char user_name[JWT_USER_NAME_LEN] = {0};
            char token[TOKEN_LEN] = {0};

            /* 登录阶段先提取用户名，再执行账号密码校验。 */
            parse_login_user_name(cmd_packet->data, user_name, sizeof(user_name));
            handle_login(client_fd, cmd_packet->data, &ctx.user_id);

            /* 只有“之前未登录、现在登录成功”时，才初始化目录状态并签发 token。 */
            if (old_user_id == -1 && ctx.user_id != -1) {
                strcpy(ctx.current_path, "/");
                ctx.current_dir_id = 0;

                /* token 生成成功时，把 token 保存到连接状态，并发送给客户端。 */
                if (jwt_create_token(ctx.user_id, user_name, JWT_EXPIRE_SECONDS, token, sizeof(token)) == 0) {
                    strncpy(state->token, token, sizeof(state->token) - 1);
                    token_packet_t token_packet;
                    init_token_packet(&token_packet, state->token, 1);
                    send_token_packet(client_fd, &token_packet);
                } else {
                    /* token 生成失败时，仍然返回一个无效 token 包，便于客户端识别异常。 */
                    token_packet_t token_packet;
                    state->token[0] = '\0';
                    init_token_packet(&token_packet, NULL, 0);
                    send_token_packet(client_fd, &token_packet);
                }
            }
            break;
        }

        case CMD_TYPE_REGISTER: {
            int temp_new_id = -1;
            /* 注册命令直接复用现有注册处理逻辑。 */
            handle_register(client_fd, cmd_packet->data, &temp_new_id);
            break;
        }

        case CMD_TYPE_PWD:
            handle_pwd(client_fd, &ctx);
            break;
        case CMD_TYPE_CD:
            handle_cd(client_fd, &ctx, cmd_packet->data);
            break;
        case CMD_TYPE_LS:
            handle_ls(client_fd, &ctx);
            break;
        case CMD_TYPE_GETS:
            /* 保留 V3 客户端兼容路径。第四期独立传输连接会优先经过认证分流。 */
            handle_gets(client_fd, &ctx, cmd_packet->data);
            break;
        case CMD_TYPE_PUTS:
            handle_puts(client_fd, &ctx, cmd_packet->data);
            break;
        case CMD_TYPE_TOUCH:
            handle_touch(client_fd, &ctx, cmd_packet->data);
            break;
        case CMD_TYPE_RM:
            handle_rm(client_fd, &ctx, cmd_packet->data);
            break;
        case CMD_TYPE_MKDIR:
            handle_mkdir(client_fd, &ctx, cmd_packet->data);
            break;
        case CMD_TYPE_RMDIR:
            handle_rmdir(client_fd, &ctx, cmd_packet->data);
            break;
        default:
            send_msg(client_fd, "指令错误!");
            break;
    }

    // 第六步：把业务处理后的上下文回写到连接状态。
    if (conn_state_sync_from_client_ctx(state, &ctx) != 0) {
        return -1;
    }

    /* 第七步：根据 user_id 同步登录标记。 */
    if (state->user_id != -1) {
        state->is_logged_in = 1;
    }

    return 0;
}

/**
 * @brief  根据认证包和后续命令，构造一个传输任务
 * @param  client_fd 当前客户端套接字
 * @param  auth_packet 已接收的认证包
 * @param  task 输出参数，用来保存最终构造的传输任务
 * @return 成功返回 0，失败返回 -1
 */
int session_build_transfer_task(int client_fd, const auth_packet_t *auth_packet, transfer_task_t *task) {
    command_packet_t cmd_packet;
    time_t exp_time = 0;
    char user_name[JWT_USER_NAME_LEN] = {0};

    // 第一步：校验输入参数。
    if (auth_packet == NULL || task == NULL) {
        return -1;
    }

    // 第二步：当前传输任务只允许 gets 和 puts。
    if (auth_packet->transfer_cmd != CMD_TYPE_GETS && auth_packet->transfer_cmd != CMD_TYPE_PUTS) {
        return -1;
    }

    // 第三步：先把任务结构体清零。
    memset(task, 0, sizeof(transfer_task_t));

    /* 第四步：校验 token，并从 token 中恢复 user_id。 */
    /* 用户名和过期时间在这里主要用于保证校验流程完整。 */
    if (jwt_verify_token(auth_packet->token,
                         &task->ctx.user_id,
                         user_name,
                         sizeof(user_name),
                         &exp_time) != 0) {
        return -1;
    }

    /* 第五步：使用认证包中的 current_path 恢复目录上下文。 */
    strncpy(task->ctx.current_path, auth_packet->current_path, sizeof(task->ctx.current_path) - 1);
    if (restore_current_dir_id(task->ctx.user_id, task->ctx.current_path, &task->ctx.current_dir_id) != 0) {
        return -1;
    }

    /* 第六步：继续接收本次传输连接上的普通命令包。 */
    if (recv_command_packet(client_fd, &cmd_packet) <= 0) {
        return -1;
    }

    /* 第七步：认证包中声明的命令类型必须与后续命令包一致。 */
    if (cmd_packet.cmd_type != auth_packet->transfer_cmd) {
        return -1;
    }

    /* 第八步：整理出线程池需要的传输任务内容。 */
    task->client_fd = client_fd;
    task->cmd_type = cmd_packet.cmd_type;
    strncpy(task->arg, cmd_packet.data, sizeof(task->arg) - 1);
    return 0;
}

/**
 * @brief  在工作线程中处理一次传输任务
 * @param  task 传输任务
 * @return 无
 */
void session_handle_transfer_task(const transfer_task_t *task) {
    ClientContext ctx;

    // 第一步：校验任务指针。
    if (task == NULL) {
        return;
    }

    // 第二步：从传输任务中恢复业务层需要的 ClientContext。
    memset(&ctx, 0, sizeof(ctx));
    ctx.user_id = task->ctx.user_id;
    ctx.current_dir_id = task->ctx.current_dir_id;
    strncpy(ctx.current_path, task->ctx.current_path, sizeof(ctx.current_path) - 1);

    /* 第三步：根据任务类型进入上传或下载处理流程。 */
    if (task->cmd_type == CMD_TYPE_GETS) {
        handle_gets(task->client_fd, &ctx, (char *)task->arg);
        return;
    }

    if (task->cmd_type == CMD_TYPE_PUTS) {
        handle_puts(task->client_fd, &ctx, (char *)task->arg);
        return;
    }

    /* 第四步：命令类型不合法时，返回统一错误消息。 */
    send_msg(task->client_fd, "传输任务无效");
}
