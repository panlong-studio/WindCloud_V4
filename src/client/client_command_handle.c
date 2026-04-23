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
 * @brief  发送普通命令并接收服务端文本响应
 * @param  sock_fd 客户端套接字
 * @param  cmd_type 命令类型
 * @param  input 用户输入的一整行命令，仅用于日志
 * @param  arg 命令参数
 * @return 成功返回服务端响应处理结果，失败返回 -1
 */
static int handle_normal_command(int sock_fd, cmd_type_t cmd_type, const char *input, const char *arg) {
    command_packet_t cmd_packet;
    init_command_packet(&cmd_packet, cmd_type, arg);

    if (send_command_packet(sock_fd, &cmd_packet) == -1) {
        printf("发送命令失败\n");
        LOG_WARN("发送命令失败，输入=%s", input);
        return -1;
    }

    // 普通命令的协议非常简单：
    // 1. 先发一个 command_packet_t
    // 2. 再收服务端回的一条文本响应
    return recv_server_reply(sock_fd);
}

/**
 * @brief  接收服务端返回的普通文本响应
 * @param  sock_fd 客户端和服务端通信的 socket
 * @return 响应中包含“成功”返回 1，普通响应返回 0，失败返回 -1
 */
int recv_server_reply(int sock_fd) {
    // reply_packet 用来保存服务端发回来的文本结构体。
    command_packet_t reply_packet;

    // 按固定结构体大小完整接收。
    if (recv_command_packet(sock_fd, &reply_packet) <= 0) {
        printf("接收服务端响应失败\n");
        LOG_WARN("接收服务端响应失败，套接字=%d", sock_fd);
        return -1;
    }

    if(strstr(reply_packet.data, "成功") != NULL) {
        return 1;
    }

    // 把服务端返回的文本直接打印出来。
    printf("%s\n", reply_packet.data);
    LOG_DEBUG("收到服务端响应，套接字=%d，消息=%s", sock_fd, reply_packet.data);
    return 0;
}

/**
 * @brief  处理用户输入的一整条命令
 * @param  sock_fd 客户端 socket
 * @param  input 用户输入的一整行命令，例如 "puts a.txt"
 * @return 根据具体命令返回处理结果
 */
int process_command(int sock_fd, const char *input) {
    client_input_t parsed;
    parse_input(input, &parsed);

    // 先做本地校验，避免把明显错误的命令发给服务端。
    if (validate_command_args(&parsed) != 0) {
        return 0;
    }

    // 把字符串命令转换成枚举命令。
    // 后面客户端和服务端通信，都靠这个编号判断命令类型。
    cmd_type_t cmd_type = get_cmd_type(parsed.cmd);
    if (cmd_type == CMD_TYPE_INVALID) {
        printf("无效命令\n");
        LOG_WARN("无效命令，输入=%s", input);
        return 0;
    }

    if (cmd_type == CMD_TYPE_GETS) {
        return handle_gets_command(sock_fd, parsed.arg);
    }

    if (cmd_type == CMD_TYPE_PUTS) {
        return handle_puts_command(sock_fd, parsed.arg);
    }

    // 除上传/下载外，其余命令都按“普通命令 + 文本响应”的协议处理。
    return handle_normal_command(sock_fd, cmd_type, input, parsed.arg);
}
