#ifndef _SESSION_H_
#define _SESSION_H_

#include "queue.h"
#include "conn_manager.h"

/**
 * @brief  向客户端发送一条普通文本消息
 * @param  client_fd 当前客户端套接字
 * @param  msg 要发送的文本内容
 * @return 无
 */
void send_msg(int client_fd, const char *msg);

/**
 * @brief  在主线程中处理一个普通命令
 * @param  client_fd 当前客户端套接字
 * @param  state 当前连接状态
 * @param  cmd_packet 客户端发送的普通命令包
 * @return 成功返回 0，连接应关闭时返回 -1
 */
int session_handle_main_command(int client_fd, ServerConnState *state, command_packet_t *cmd_packet);

/**
 * @brief  根据认证包和后续命令构造一条传输任务
 * @param  client_fd 当前客户端套接字
 * @param  auth_packet 客户端发送的认证包
 * @param  task 输出参数，用来保存构造好的传输任务
 * @return 成功返回 0，失败返回 -1
 */
int session_build_transfer_task(int client_fd, const auth_packet_t *auth_packet, transfer_task_t *task);

/**
 * @brief  在工作线程中处理一次传输任务
 * @param  task 待执行的传输任务
 * @return 无
 */
void session_handle_transfer_task(const transfer_task_t *task);

#endif
