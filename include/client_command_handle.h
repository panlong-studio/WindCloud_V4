#ifndef CLIENT_COMMAND_HANDLE_H
#define CLIENT_COMMAND_HANDLE_H

#include "client_state.h"

// 解析一整行用户输入，并把命令分发到普通命令 / 上传 / 下载处理函数。
int process_command(ClientState *state, const char *input);

// 接收服务端返回的普通文本响应。
// 返回 1 表示消息中包含“成功”，0 表示普通响应，-1 表示接收失败。
int recv_server_reply(int sock_fd, command_packet_t *reply_packet);

// 处理客户端下载命令。
int handle_gets_command(ClientState *state, const char *arg);

// 处理客户端上传命令。
int handle_puts_command(ClientState *state, const char *arg);

// 在控制连接上为本次上传或下载申请一次性传输票据。
int request_transfer_ticket(ClientState *state, cmd_type_t cmd_type, const char *arg,
                            char *ticket, size_t ticket_size);

// 在控制连接上申请多点下载方案。
int request_multi_gets_plan(ClientState *state, const char *arg, multi_gets_plan_packet_t *plan_packet);

// 在控制连接上为某个下载分片申请区间票据。
int request_gets_range_ticket(ClientState *state, const char *file_name,
                              off_t range_start, off_t range_end,
                              const char *source_ip, const char *source_port,
                              char *ticket, size_t ticket_size);

#endif
