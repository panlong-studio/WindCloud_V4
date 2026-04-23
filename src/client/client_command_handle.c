#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "client_command_handle.h"
#include "protocol.h"
#include "log.h"

typedef struct {
    char cmd[100];
    char arg[200];
} client_input_t;

/**
 * @brief  把客户端上下文恢复到未登录的初始状态
 * @param  ctx 客户端上下文
 * @return 无
 */
static void reset_client_login_state(ClientAppContext *ctx) {
    if (ctx == NULL) {
        return;
    }

    /* 清理登录标记、当前路径和 token。 */
    ctx->is_logged_in = 0;
    strcpy(ctx->current_path, "/");
    ctx->token[0] = '\0';
}

/**
 * @brief  根据 cd 参数计算新的客户端路径
 * @param  current_path 当前路径
 * @param  arg 用户输入参数
 * @param  output_path 输出参数，用来保存新路径
 * @param  output_size output_path 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int build_next_cd_path(const char *current_path, const char *arg, char *output_path, size_t output_size) {
    char tmp[CMD_DATA_LEN] = {0};
    char *last_slash = NULL;

    if (current_path == NULL || arg == NULL || output_path == NULL || output_size == 0) {
        return -1;
    }

    /* cd . 表示保持当前路径不变。 */
    if (strcmp(arg, ".") == 0) {
        strncpy(output_path, current_path, output_size - 1);
        return 0;
    }

    /* cd .. 表示返回上一级目录。 */
    if (strcmp(arg, "..") == 0) {
        if (strcmp(current_path, "/") == 0) {
            strncpy(output_path, "/", output_size - 1);
            return 0;
        }

        strncpy(tmp, current_path, sizeof(tmp) - 1);
        last_slash = strrchr(tmp, '/');
        if (last_slash == tmp) {
            strncpy(output_path, "/", output_size - 1);
            return 0;
        }
        if (last_slash != NULL) {
            *last_slash = '\0';
        }
        strncpy(output_path, tmp, output_size - 1);
        return 0;
    }

    /* 绝对路径直接作为新的当前路径。 */
    if (arg[0] == '/') {
        strncpy(output_path, arg, output_size - 1);
        return 0;
    }

    /* 相对路径需要拼接到当前路径后面。 */
    if (strcmp(current_path, "/") == 0) {
        snprintf(output_path, output_size, "/%s", arg);
    } else {
        snprintf(output_path, output_size, "%s/%s", current_path, arg);
    }

    return 0;
}

/**
 * @brief  把用户输入拆成命令字和参数
 * @param  input 用户输入的一整行命令
 * @param  parsed 输出参数，用来保存拆分后的命令和参数
 * @return 无
 */
static void parse_input(const char *input, client_input_t *parsed) {
    /* 先清空结构体，再用 sscanf 提取命令字和参数。 */
    memset(parsed, 0, sizeof(*parsed));
    sscanf(input, "%99s %199s", parsed->cmd, parsed->arg);
}

/**
 * @brief  在客户端发送前校验命令参数是否合法
 * @param  parsed 已经拆分完成的客户端输入
 * @return 校验通过返回 0，校验失败返回 -1
 */
static int validate_command_args(const client_input_t *parsed) {
    /* gets 和 puts 都要求带文件名。 */
    if (strcmp(parsed->cmd, "gets") == 0 && parsed->arg[0] == '\0') {
        printf("用法: gets <文件名>\n");
        return -1;
    }

    if (strcmp(parsed->cmd, "puts") == 0 && parsed->arg[0] == '\0') {
        printf("用法: puts <文件名>\n");
        return -1;
    }

    /* 需要路径参数的普通命令必须带参数。 */
    if ((strcmp(parsed->cmd, "cd") == 0 ||
         strcmp(parsed->cmd, "rm") == 0 ||
         strcmp(parsed->cmd, "mkdir") == 0 ||
         strcmp(parsed->cmd, "touch") == 0 ||
         strcmp(parsed->cmd, "rmdir") == 0) &&
        parsed->arg[0] == '\0') {
        printf("该命令需要参数\n");
        return -1;
    }

    /* 文件名和目录名长度统一按虚拟文件系统规则校验。 */
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
 * @brief  初始化客户端上下文
 * @param  ctx 客户端上下文
 * @param  sock_fd 主连接 fd
 * @param  ip 服务端 IP
 * @param  port 服务端端口
 * @return 无
 */
void client_app_context_init(ClientAppContext *ctx, int sock_fd, const char *ip, const char *port) {
    if (ctx == NULL) {
        return;
    }

    /* 第一步：整体清零，避免残留旧状态。 */
    memset(ctx, 0, sizeof(ClientAppContext));
    ctx->sock_fd = sock_fd;
    strcpy(ctx->current_path, "/");

    /* 第二步：保存服务端地址，后续重连和传输线程都会复用。 */
    if (ip != NULL) {
        strncpy(ctx->server_ip, ip, sizeof(ctx->server_ip) - 1);
    }

    if (port != NULL) {
        strncpy(ctx->server_port, port, sizeof(ctx->server_port) - 1);
    }
}

/**
 * @brief  接收服务端返回的普通文本响应
 * @param  sock_fd 客户端和服务端通信的 socket
 * @param  reply_packet 输出参数，用来保存响应包，可以传 NULL
 * @return 响应中包含“成功”返回 1，普通响应返回 0，失败返回 -1
 */
int recv_server_reply(int sock_fd, command_packet_t *reply_packet) {
    command_packet_t local_reply_packet;

    /* 第一步：接收服务端返回的普通命令包。 */
    if (recv_command_packet(sock_fd, &local_reply_packet) <= 0) {
        printf("接收服务端响应失败\n");
        LOG_WARN("接收服务端响应失败，套接字=%d", sock_fd);
        return -1;
    }

    /* 第二步：如果调用方需要，就把响应包再拷贝出去。 */
    if (reply_packet != NULL) {
        memcpy(reply_packet, &local_reply_packet, sizeof(command_packet_t));
    }

    /* 第三步：输出服务端文本提示。 */
    printf("%s\n", local_reply_packet.data);
    LOG_DEBUG("收到服务端响应，套接字=%d，消息=%s", sock_fd, local_reply_packet.data);

    /* 根据响应文本中是否含有“成功”，向上层返回简单结果标记。 */
    if (strstr(local_reply_packet.data, "成功") != NULL) {
        return 1;
    }

    return 0;
}

/**
 * @brief  登录成功后接收服务端返回的 token
 * @param  ctx 客户端上下文
 * @return 成功返回 0，失败返回 -1
 */
static int recv_login_token(ClientAppContext *ctx) {
    token_packet_t token_packet;

    if (ctx == NULL) {
        return -1;
    }

    /* 第一步：读取服务端在登录成功后追加发送的 token 包。 */
    if (recv_token_packet(ctx->sock_fd, &token_packet) <= 0) {
        printf("接收 token 失败\n");
        LOG_WARN("接收 token 失败，套接字=%d", ctx->sock_fd);
        return -1;
    }

    /* 第二步：确认 token 包内容有效。 */
    if (token_packet.is_ok != 1 || token_packet.token[0] == '\0') {
        printf("登录失败：服务端没有返回有效 token\n");
        LOG_WARN("服务端未返回有效 token，套接字=%d", ctx->sock_fd);
        return -1;
    }

    /* 第三步：把 token 保存到客户端上下文，并更新登录状态。 */
    strncpy(ctx->token, token_packet.token, sizeof(ctx->token) - 1);
    ctx->is_logged_in = 1;
    strcpy(ctx->current_path, "/");
    return 0;
}

/**
 * @brief  发送普通命令，并根据命令类型更新客户端状态
 * @param  ctx 客户端上下文
 * @param  cmd_type 命令类型
 * @param  input 用户输入原文
 * @param  arg 命令参数
 * @return 成功返回 1 或 0，失败返回 -1
 */
static int handle_normal_command(ClientAppContext *ctx, cmd_type_t cmd_type, const char *input, const char *arg) {
    command_packet_t cmd_packet;
    command_packet_t reply_packet;
    int ret = 0;

    if (ctx == NULL) {
        return -1;
    }

    /* 第一步：把命令类型和参数打包成统一协议。 */
    init_command_packet(&cmd_packet, cmd_type, arg);

    /* 第二步：通过主连接发送普通命令。 */
    if (send_command_packet(ctx->sock_fd, &cmd_packet) == -1) {
        printf("发送命令失败\n");
        LOG_WARN("发送命令失败，输入=%s", input);
        reset_client_login_state(ctx);
        return -1;
    }

    /* 第三步：接收服务端返回的执行结果。 */
    ret = recv_server_reply(ctx->sock_fd, &reply_packet);
    if (ret == -1) {
        reset_client_login_state(ctx);
        return -1;
    }

    /* 登录成功后，继续接收并保存服务端下发的 token。 */
    if (cmd_type == CMD_TYPE_LOGIN && ret == 1) {
        if (recv_login_token(ctx) != 0) {
            reset_client_login_state(ctx);
            return -1;
        }
    }

    /* cd 成功后，同步刷新客户端维护的当前路径。 */
    if (cmd_type == CMD_TYPE_CD && ret == 1) {
        char next_path[CMD_DATA_LEN] = {0};
        if (build_next_cd_path(ctx->current_path, arg, next_path, sizeof(next_path)) == 0) {
            strncpy(ctx->current_path, next_path, sizeof(ctx->current_path) - 1);
        }
    }

    /* 登录失败时，确保客户端仍保持未登录状态。 */
    if (cmd_type == CMD_TYPE_LOGIN && ret != 1) {
        reset_client_login_state(ctx);
    }

    return ret;
}

/**
 * @brief  处理用户输入的一整条命令
 * @param  ctx 客户端上下文
 * @param  input 用户输入的一整行命令，例如 "puts a.txt"
 * @return 根据具体命令返回处理结果
 */
int process_command(ClientAppContext *ctx, const char *input) {
    client_input_t parsed;
    cmd_type_t cmd_type;

    if (ctx == NULL) {
        return -1;
    }

    /* 第一步：把输入拆成命令字和参数。 */
    parse_input(input, &parsed);

    /* 第二步：先做本地参数校验，尽量在发送前发现问题。 */
    if (validate_command_args(&parsed) != 0) {
        return 0;
    }

    /* 第三步：把命令字转换成协议中的命令类型。 */
    cmd_type = get_cmd_type(parsed.cmd);
    if (cmd_type == CMD_TYPE_INVALID) {
        printf("无效命令\n");
        LOG_WARN("无效命令，输入=%s", input);
        return 0;
    }

    /* gets 走独立下载连接。 */
    if (cmd_type == CMD_TYPE_GETS) {
        return handle_gets_command(ctx, parsed.arg);
    }

    /* puts 走独立上传连接。 */
    if (cmd_type == CMD_TYPE_PUTS) {
        return handle_puts_command(ctx, parsed.arg);
    }

    /* 其它命令继续使用主连接处理。 */
    return handle_normal_command(ctx, cmd_type, input, parsed.arg);
}
