#ifndef _SERVER_IDENTITY_H_
#define _SERVER_IDENTITY_H_

#include <stddef.h>

/**
 * @brief  读取当前服务端自己的 IP 和端口
 * @param  ip 输出参数，用来保存服务端 IP
 * @param  ip_size ip 缓冲区大小
 * @param  port 输出参数，用来保存服务端端口
 * @param  port_size port 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_current_server_address(char *ip, size_t ip_size, char *port, size_t port_size);

#endif
