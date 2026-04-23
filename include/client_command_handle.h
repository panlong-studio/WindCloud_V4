#ifndef CLIENT_COMMAND_HANDLE_H
#define CLIENT_COMMAND_HANDLE_H

// 解析一整行用户输入，并把命令分发到普通命令 / 上传 / 下载处理函数。
int process_command(int sock_fd, const char *input);

// 接收服务端返回的普通文本响应。
// 返回 1 表示消息中包含“成功”，0 表示普通响应，-1 表示接收失败。
int recv_server_reply(int sock_fd);

// 处理客户端下载命令。
int handle_gets_command(int sock_fd, const char *arg);

// 处理客户端上传命令。
int handle_puts_command(int sock_fd, const char *arg);

#endif
