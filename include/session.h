#ifndef _SESSION_H_
#define _SESSION_H_

// 向客户端发送一条普通文本消息
void send_msg(int client_fd, const char *msg);

// 处理一个客户端连接上的所有请求（主循环）
void handle_request(int client_fd);

#endif