#ifndef _SOCKET_H_
#define _SOCKET_H_

/**
 * @brief  创建客户端 socket，并主动连接到目标服务端
 * @param  fd 输出参数，用来保存创建好的客户端 socket fd
 * @param  ip 服务端 IP 地址字符串
 * @param  port 服务端端口字符串
 * @return 无
 */
void init_socket(int* fd,char* ip,char* port);

#endif
