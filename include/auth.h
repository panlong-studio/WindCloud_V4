#ifndef _AUTH_H_
#define _AUTH_H_

/**
 * @brief  处理登录命令，并向客户端返回登录结果与 JWT
 * @param  client_fd 当前客户端套接字
 * @param  data 客户端发送的登录参数，格式为 "用户名/密码"
 * @param  user_id 输出参数，登录成功时返回用户 id，失败返回 -1
 * @return 无
 */
void handle_login(int client_fd, const char *data, int *user_id);

/**
 * @brief  处理注册命令
 * @param  client_fd 当前客户端套接字
 * @param  data 客户端发送的注册参数，格式为 "用户名/密码"
 * @param  user_id 输出参数，注册成功时返回用户 id，失败返回 -1
 * @return 无
 */
void handle_register(int client_fd, const char *data, int *user_id);

#endif
