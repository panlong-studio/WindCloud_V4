#ifndef _SESSION_H_
#define _SESSION_H_

#include "protocol.h"

// 向客户端发送一条普通文本消息
void send_msg(int client_fd, const char *msg);

// 处理一条控制连接上的单个命令包
void dispatch_control_command(int client_fd, ClientContext *ctx, const command_packet_t *cmd_packet);

// 处理一条传输连接上的完整请求
void handle_transfer_request(int client_fd);

// 处理一个客户端连接上的所有请求（兼容旧逻辑）
void handle_request(int client_fd);

#endif
