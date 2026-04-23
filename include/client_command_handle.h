#ifndef CLIENT_COMMAND_HANDLE_H
#define CLIENT_COMMAND_HANDLE_H

#include "protocol.h"

typedef struct {
    int sock_fd;
    int is_logged_in;
    char current_path[CMD_DATA_LEN];
    char token[TOKEN_LEN];
    char server_ip[64];
    char server_port[32];
} ClientAppContext;

/**
 * @brief  初始化客户端上下文
 * @param  ctx 客户端上下文
 * @param  sock_fd 主连接 fd
 * @param  ip 服务端 IP
 * @param  port 服务端端口
 * @return 无
 */
void client_app_context_init(ClientAppContext *ctx, int sock_fd, const char *ip, const char *port);

/**
 * @brief  解析一整行用户输入，并分发到对应处理函数
 * @param  ctx 客户端上下文
 * @param  input 用户输入的一整行命令
 * @return 成功返回 1 或 0，失败返回 -1
 */
int process_command(ClientAppContext *ctx, const char *input);

/**
 * @brief  接收服务端返回的普通文本响应
 * @param  sock_fd 客户端和服务端通信的 socket
 * @param  reply_packet 输出参数，用来保存响应包，可以传 NULL
 * @return 响应中包含“成功”返回 1，普通响应返回 0，失败返回 -1
 */
int recv_server_reply(int sock_fd, command_packet_t *reply_packet);

/**
 * @brief  处理客户端 gets 命令
 * @param  ctx 客户端上下文
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_gets_command(ClientAppContext *ctx, const char *arg);

/**
 * @brief  处理客户端 puts 命令
 * @param  ctx 客户端上下文
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_puts_command(ClientAppContext *ctx, const char *arg);

#endif
