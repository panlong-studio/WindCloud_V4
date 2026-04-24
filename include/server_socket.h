#ifndef SERVER_SOCKET_H_
#define SERVER_SOCKET_H_

/**
 * @brief  初始化服务端监听套接字
 * @param  fd 输出参数，用来保存初始化完成后的监听 fd
 * @param  ip 服务端绑定的 IP 地址字符串
 * @param  port 服务端绑定的端口字符串
 * @return 无
 */
void init_socket(int* fd,char* ip,char* port);

#endif
