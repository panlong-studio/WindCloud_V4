#ifndef _FILE_CMDS_H_
#define _FILE_CMDS_H_

#include "protocol.h"

/**
 * @brief  处理 cd 命令，修改客户端当前虚拟路径
 * @param  client_fd 客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的目标目录
 * @return 无
 */
void handle_cd(int client_fd, ClientContext *ctx, char *arg);

/**
 * @brief  处理 ls 命令，列出当前目录内容
 * @param  client_fd 客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @return 无
 */
void handle_ls(int client_fd, ClientContext *ctx);

/**
 * @brief  处理 pwd 命令，返回当前虚拟路径
 * @param  client_fd 客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @return 无
 */
void handle_pwd(int client_fd, ClientContext *ctx);

void handle_touch(int client_fd, ClientContext *ctx, char *arg);

/**
 * @brief  处理 rm 命令，删除当前目录下的普通文件节点
 * @param  client_fd 客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_rm(int client_fd, ClientContext *ctx, char *arg);

/**
 * @brief  处理 mkdir 命令，在当前目录下创建目录
 * @param  client_fd 客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的目录名
 * @return 无
 */
void handle_mkdir(int client_fd, ClientContext *ctx, char *arg);

void handle_rmdir(int client_fd, ClientContext *ctx, char *arg);

#endif
