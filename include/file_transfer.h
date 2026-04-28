#ifndef _FILE_TRANSFER_H_
#define _FILE_TRANSFER_H_

#include "protocol.h"

/**
 * @brief  处理 gets 命令，也就是下载文件
 * @param  client_fd 客户端连接 fd
 * @param  ctx 当前客户端会话上下文，里面保存 user_id、当前路径、current_dir_id
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_gets(int client_fd, ClientContext *ctx, char *arg);

/**
 * @brief  处理指定字节区间的 gets 下载请求
 * @param  client_fd 客户端连接 fd
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @param  range_start 本次允许发送的起始位置
 * @param  range_end 本次允许发送的结束位置
 * @return 无
 */
void handle_gets_range(int client_fd, ClientContext *ctx, char *arg, off_t range_start, off_t range_end);

/**
 * @brief  处理 puts 命令，也就是上传文件和秒传
 * @param  client_fd 客户端连接 fd
 * @param  ctx 当前客户端会话上下文，里面保存 user_id、当前路径、current_dir_id
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_puts(int client_fd, ClientContext *ctx, char *arg);

#endif
