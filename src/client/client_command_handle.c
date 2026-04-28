#include <stdio.h>
#include <string.h>
#include "client_command_handle.h"
#include "protocol.h"
#include "log.h"

typedef struct {
    char cmd[100];
    char arg[200];
} client_input_t;

/**
 * @brief  把用户输入拆成命令字和参数
 * @param  input 用户输入的一整行命令
 * @param  parsed 输出参数，用来保存拆分后的命令和参数
 * @return 无
 */
static void parse_input(const char *input, client_input_t *parsed) {
    memset(parsed, 0, sizeof(*parsed));
    sscanf(input, "%99s %199s", parsed->cmd, parsed->arg);
}

/**
 * @brief  获取客户端当前记录的虚拟路径快照
 * @param  state 客户端统一状态结构体
 * @param  path 输出参数，用来保存当前路径
 * @param  size path 缓冲区大小
 * @return 无
 */
static void get_current_path_snapshot(ClientState *state, char *path, size_t size) {
    pthread_mutex_lock(&state->lock);
    snprintf(path, size, "%s", state->current_path);
    pthread_mutex_unlock(&state->lock);
}

/**
 * @brief  登录成功后更新客户端状态
 * @param  state 客户端统一状态结构体
 * @param  user_id 登录成功后的用户 id
 * @param  token 服务端返回的 JWT
 * @return 无
 */
static void set_login_success_state(ClientState *state, int user_id, const char *token) {
    pthread_mutex_lock(&state->lock);
    state->logged_in = 1;
    state->user_id = user_id;
    snprintf(state->token, sizeof(state->token), "%s", token);
    snprintf(state->current_path, sizeof(state->current_path), "%s", "/");
    pthread_mutex_unlock(&state->lock);
}

/**
 * @brief  根据 cd 参数在客户端本地更新当前虚拟路径
 * @param  state 客户端统一状态结构体
 * @param  arg cd 命令参数
 * @return 无
 */
static void update_client_path_after_cd(ClientState *state, const char *arg) {
    char current_path[256] = {0};
    char next_path[512] = {0};
    char *last_slash = NULL;

    get_current_path_snapshot(state, current_path, sizeof(current_path));

    if (strcmp(arg, ".") == 0) {
        return;
    }

    if (strcmp(arg, "..") == 0) {
        if (strcmp(current_path, "/") == 0) {
            return;
        }

        snprintf(next_path, sizeof(next_path), "%s", current_path);
        last_slash = strrchr(next_path, '/');
        if (last_slash == next_path) {
            snprintf(next_path, sizeof(next_path), "%s", "/");
        } else if (last_slash != NULL) {
            *last_slash = '\0';
        }
    } else if (arg[0] == '/') {
        snprintf(next_path, sizeof(next_path), "%s", arg);
    } else if (strcmp(current_path, "/") == 0) {
        snprintf(next_path, sizeof(next_path), "/%s", arg);
    } else {
        snprintf(next_path, sizeof(next_path), "%s/%s", current_path, arg);
    }

    if (strlen(next_path) >= sizeof(state->current_path)) {
        return;
    }

    pthread_mutex_lock(&state->lock);
    strcpy(state->current_path, next_path);
    pthread_mutex_unlock(&state->lock);
}

/**
 * @brief  在客户端发送前校验命令参数是否合法
 * @param  parsed 已经拆分完成的客户端输入
 * @return 校验通过返回 0，校验失败返回 -1
 */
static int validate_command_args(const client_input_t *parsed) {
    // gets / puts 必须带文件名。
    // 如果这里不拦，后面服务端虽然也能拒绝，但会多走一次无意义的网络往返。
    if (strcmp(parsed->cmd, "gets") == 0 && parsed->arg[0] == '\0') {
        printf("用法: gets <文件名>\n");
        return -1;
    }

    if (strcmp(parsed->cmd, "puts") == 0 && parsed->arg[0] == '\0') {
        printf("用法: puts <文件名>\n");
        return -1;
    }

    // 这些命令语义上都依赖一个目标参数。
    // 客户端先拦截，能让用户更快看到明确提示，也能减少服务端无效处理。
    if ((strcmp(parsed->cmd, "cd") == 0 ||
         strcmp(parsed->cmd, "rm") == 0 ||
         strcmp(parsed->cmd, "mkdir") == 0 ||
         strcmp(parsed->cmd, "touch") == 0 ||
         strcmp(parsed->cmd, "rmdir") == 0) &&
        parsed->arg[0] == '\0') {
        printf("该命令需要参数\n");
        return -1;
    }

    // paths.file_name 在数据库里的上限是 30。
    // 这里和服务端共用 MAX_VFS_NAME_LEN，避免长度限制在两端出现分叉。
    // 客户端提前拒绝后，就不会再把明显非法的请求送到服务端。
    if ((strcmp(parsed->cmd, "puts") == 0 ||
         strcmp(parsed->cmd, "gets") == 0 ||
         strcmp(parsed->cmd, "mkdir") == 0 ||
         strcmp(parsed->cmd, "touch") == 0 ||
         strcmp(parsed->cmd, "rm") == 0 ||
         strcmp(parsed->cmd, "rmdir") == 0) &&
        strlen(parsed->arg) > MAX_VFS_NAME_LEN) {
        printf("文件名或目录名过长，最大长度为 30\n");
        return -1;
    }

    return 0;
}

/**
 * @brief  发送登录命令并接收服务端返回的登录结果
 * @param  state 客户端统一状态结构体
 * @param  arg 登录参数，格式为 "用户名/密码"
 * @return 成功返回 1，失败返回 0 或 -1
 */
static int handle_login_command(ClientState *state, const char *arg) {
    command_packet_t cmd_packet;
    auth_reply_packet_t reply_packet;

    init_command_packet(&cmd_packet, CMD_TYPE_LOGIN, arg);
    if (send_command_packet(state->ctrl_fd, &cmd_packet) == -1) {
        printf("发送登录命令失败\n");
        LOG_WARN("发送登录命令失败");
        return -1;
    }

    if (recv_auth_reply_packet(state->ctrl_fd, &reply_packet) <= 0) {
        printf("接收登录结果失败\n");
        LOG_WARN("接收登录结果失败");
        return -1;
    }

    printf("%s\n", reply_packet.message);
    if (reply_packet.success != 1) {
        return 0;
    }

    set_login_success_state(state, reply_packet.user_id, reply_packet.token);
    return 1;
}

/**
 * @brief  发送普通命令并接收服务端文本响应
 * @param  state 客户端统一状态结构体
 * @param  cmd_type 命令类型
 * @param  input 用户输入的一整行命令，仅用于日志
 * @param  arg 命令参数
 * @return 成功返回服务端响应处理结果，失败返回 -1
 */
static int handle_normal_command(ClientState *state, cmd_type_t cmd_type, const char *input, const char *arg) {
    command_packet_t cmd_packet;
    command_packet_t reply_packet;
    int ret = 0;

    init_command_packet(&cmd_packet, cmd_type, arg);

    if (send_command_packet(state->ctrl_fd, &cmd_packet) == -1) {
        printf("发送命令失败\n");
        LOG_WARN("发送命令失败，输入=%s", input);
        return -1;
    }

    // 普通命令统一走“命令包 -> 文本响应”的协议。
    ret = recv_server_reply(state->ctrl_fd, &reply_packet);
    if (ret == -1) {
        return -1;
    }

    printf("%s\n", reply_packet.data);
    LOG_DEBUG("收到服务端响应，套接字=%d，消息=%s", state->ctrl_fd, reply_packet.data);

    // cd 成功后，客户端需要同步更新自己记录的当前路径，
    // 后面传输线程要依赖这个路径拼完整虚拟路径。
    if (cmd_type == CMD_TYPE_CD && ret == 1) {
        update_client_path_after_cd(state, arg);
    }

    return ret;
}

/**
 * @brief  在控制连接上为 puts/gets 申请一次性传输票据
 * @param  state 客户端统一状态结构体
 * @param  cmd_type 传输命令类型
 * @param  arg 用户输入的文件名
 * @param  ticket 输出参数，用来保存服务端返回的传输票据
 * @param  ticket_size ticket 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int request_transfer_ticket(ClientState *state, cmd_type_t cmd_type, const char *arg,
                            char *ticket, size_t ticket_size) {
    command_packet_t cmd_packet;
    transfer_ticket_reply_packet_t reply_packet;

    if (state == NULL || arg == NULL || ticket == NULL || ticket_size == 0) {
        return -1;
    }

    init_command_packet(&cmd_packet, cmd_type, arg);
    if (send_command_packet(state->ctrl_fd, &cmd_packet) == -1) {
        printf("发送传输预请求失败\n");
        LOG_WARN("发送传输预请求失败，命令类型=%d，参数=%s", cmd_type, arg);
        return -1;
    }

    if (recv_transfer_ticket_reply_packet(state->ctrl_fd, &reply_packet) <= 0) {
        printf("接收传输票据失败\n");
        LOG_WARN("接收传输票据失败，命令类型=%d，参数=%s", cmd_type, arg);
        return -1;
    }

    printf("%s\n", reply_packet.message);
    if (reply_packet.success != 1) {
        return -1;
    }

    snprintf(ticket, ticket_size, "%s", reply_packet.ticket);
    return 0;
}

/**
 * @brief  在控制连接上申请多点下载方案
 * @param  state 客户端统一状态结构体
 * @param  arg 用户输入的文件名
 * @param  plan_packet 输出参数，用来保存服务端返回的多点下载方案
 * @return 成功返回 0，失败返回 -1
 */
int request_multi_gets_plan(ClientState *state, const char *arg, multi_gets_plan_packet_t *plan_packet) {
    command_packet_t cmd_packet;

    if (state == NULL || arg == NULL || plan_packet == NULL) {
        return -1;
    }

    // 第五期后，控制连接上的 gets 不再直接传数据，
    // 而是先向服务端申请“文件有多大、有哪些数据源可用”这份下载方案。
    init_command_packet(&cmd_packet, CMD_TYPE_GETS, arg);
    if (send_command_packet(state->ctrl_fd, &cmd_packet) == -1) {
        printf("发送多点下载方案请求失败\n");
        LOG_WARN("发送多点下载方案请求失败，参数=%s", arg);
        return -1;
    }

    // 服务端这里返回的是专门的多点下载方案结构，不再是普通文本响应。
    if (recv_multi_gets_plan_packet(state->ctrl_fd, plan_packet) <= 0) {
        printf("接收多点下载方案失败\n");
        LOG_WARN("接收多点下载方案失败，参数=%s", arg);
        return -1;
    }

    printf("%s\n", plan_packet->message);
    if (plan_packet->success != 1) {
        return -1;
    }

    return 0;
}

/**
 * @brief  把分片下载票据申请参数拼成控制连接可发送的字符串
 * @param  request_buf 输出参数，用来保存最终请求字符串
 * @param  request_size request_buf 缓冲区大小
 * @param  file_name 目标文件名
 * @param  range_start 本分片起始位置
 * @param  range_end 本分片结束位置
 * @param  source_ip 数据源服务器 IP
 * @param  source_port 数据源服务器端口
 * @return 成功返回 0，失败返回 -1
 */
static int build_gets_range_ticket_request(char *request_buf, size_t request_size,
                                           const char *file_name,
                                           off_t range_start, off_t range_end,
                                           const char *source_ip, const char *source_port) {
    if (request_buf == NULL || request_size == 0 ||
        file_name == NULL || source_ip == NULL || source_port == NULL) {
        return -1;
    }

    // 第五期为了保持控制连接主循环简单，区间票据申请仍然复用 command_packet_t。
    // 这里把文件名、区间和目标数据源编码成一个固定分隔格式的字符串。
    if (snprintf(request_buf, request_size, "%s|%lld|%lld|%s|%s",
                 file_name,
                 (long long)range_start,
                 (long long)range_end,
                 source_ip,
                 source_port) >= (int)request_size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  在控制连接上为某个下载分片申请区间票据
 * @param  state 客户端统一状态结构体
 * @param  file_name 目标文件名
 * @param  range_start 本分片起始位置
 * @param  range_end 本分片结束位置
 * @param  source_ip 数据源服务器 IP
 * @param  source_port 数据源服务器端口
 * @param  ticket 输出参数，用来保存服务端返回的区间票据
 * @param  ticket_size ticket 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int request_gets_range_ticket(ClientState *state, const char *file_name,
                              off_t range_start, off_t range_end,
                              const char *source_ip, const char *source_port,
                              char *ticket, size_t ticket_size) {
    command_packet_t cmd_packet;
    transfer_ticket_reply_packet_t reply_packet;
    char request_buf[CMD_DATA_LEN] = {0};

    if (state == NULL || file_name == NULL ||
        source_ip == NULL || source_port == NULL ||
        ticket == NULL || ticket_size == 0) {
        return -1;
    }

    // 先把“文件名 + 分片区间 + 目标数据源”编码成一段字符串。
    // 控制连接主循环只需要识别命令类型，再按约定格式拆开即可。
    if (build_gets_range_ticket_request(request_buf,
                                        sizeof(request_buf),
                                        file_name,
                                        range_start,
                                        range_end,
                                        source_ip,
                                        source_port) != 0) {
        return -1;
    }

    // CMD_TYPE_GETS_RANGE 表示“不是立刻下载，而是先申请一个分片票据”。
    init_command_packet(&cmd_packet, CMD_TYPE_GETS_RANGE, request_buf);
    if (send_command_packet(state->ctrl_fd, &cmd_packet) == -1) {
        printf("发送下载分片票据请求失败\n");
        LOG_WARN("发送下载分片票据请求失败，文件=%s，区间=%lld-%lld",
                 file_name,
                 (long long)range_start,
                 (long long)range_end);
        return -1;
    }

    if (recv_transfer_ticket_reply_packet(state->ctrl_fd, &reply_packet) <= 0) {
        printf("接收下载分片票据失败\n");
        LOG_WARN("接收下载分片票据失败，文件=%s，区间=%lld-%lld",
                 file_name,
                 (long long)range_start,
                 (long long)range_end);
        return -1;
    }

    if (reply_packet.success != 1) {
        printf("%s\n", reply_packet.message);
        return -1;
    }

    // 申请成功后，把票据拷贝给分片下载线程使用。
    snprintf(ticket, ticket_size, "%s", reply_packet.ticket);
    return 0;
}

/**
 * @brief  接收服务端返回的普通文本响应
 * @param  sock_fd 客户端和服务端通信的 socket
 * @param  reply_packet 输出参数，用来保存服务端响应
 * @return 响应中包含“成功”返回 1，普通响应返回 0，失败返回 -1
 */
int recv_server_reply(int sock_fd, command_packet_t *reply_packet) {
    command_packet_t local_packet;
    command_packet_t *target_packet = reply_packet;

    if (target_packet == NULL) {
        target_packet = &local_packet;
    }

    // 按固定结构体大小完整接收。
    if (recv_command_packet(sock_fd, target_packet) <= 0) {
        printf("接收服务端响应失败\n");
        LOG_WARN("接收服务端响应失败，套接字=%d", sock_fd);
        return -1;
    }

    if(strstr(target_packet->data, "成功") != NULL) {
        return 1;
    }

    return 0;
}

/**
 * @brief  处理用户输入的一整条命令
 * @param  state 客户端统一状态结构体
 * @param  input 用户输入的一整行命令，例如 "puts a.txt"
 * @return 根据具体命令返回处理结果
 */
int process_command(ClientState *state, const char *input) {
    client_input_t parsed;
    cmd_type_t cmd_type;

    parse_input(input, &parsed);

    // 先做本地校验，避免把明显错误的命令发给服务端。
    if (validate_command_args(&parsed) != 0) {
        return 0;
    }

    // 把字符串命令转换成枚举命令。
    // 后面客户端和服务端通信，都靠这个编号判断命令类型。
    cmd_type = get_cmd_type(parsed.cmd);
    if (cmd_type == CMD_TYPE_INVALID) {
        printf("无效命令\n");
        LOG_WARN("无效命令，输入=%s", input);
        return 0;
    }

    if (cmd_type == CMD_TYPE_LOGIN) {
        return handle_login_command(state, parsed.arg);
    }

    if (cmd_type == CMD_TYPE_GETS) {
        return handle_gets_command(state, parsed.arg);
    }

    if (cmd_type == CMD_TYPE_PUTS) {
        return handle_puts_command(state, parsed.arg);
    }

    // 除上传、下载、登录外，其余命令都按普通文本响应处理。
    return handle_normal_command(state, cmd_type, input, parsed.arg);
}
