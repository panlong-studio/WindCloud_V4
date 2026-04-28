#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "server_socket.h"
#include "error_check.h"
#include "log.h"

/**
 * @brief  创建服务端监听 socket，并完成 bind + listen
 * @param  fd 输出参数，用来保存监听 socket
 * @param  ip 监听 IP 地址字符串
 * @param  port 监听端口字符串
 * @return 无
 */
void init_socket(int* fd,char* ip,char* port){
    // 创建 TCP 监听 socket。
    *fd=socket(AF_INET,SOCK_STREAM,0);
    ERROR_CHECK(*fd,-1,"创建服务端监听套接字");
    LOG_INFO("服务端监听套接字创建成功，描述符=%d", *fd);

    // SO_REUSEADDR 允许端口快速复用。
    // 这样服务端重启时，不容易遇到“地址已被占用”。
    int opt=1;
    if (setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        LOG_WARN("设置 SO_REUSEADDR 失败，描述符=%d，错误码=%d", *fd, errno);
    }

    // addr 保存服务端的监听地址。
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));

    // 使用 IPv4。
    addr.sin_family=AF_INET;

    // 端口字符串先转整数，再转网络字节序。
    addr.sin_port=htons(atoi(port));

    // 把字符串 IP 转成网络字节序地址。
    addr.sin_addr.s_addr=inet_addr(ip);

    // bind 的作用是把 socket 和某个 IP + 端口绑定起来。
    int ret=bind(*fd,(struct sockaddr*)&addr,sizeof(addr));
    ERROR_CHECK(ret,-1,"绑定监听地址");
    LOG_INFO("服务端绑定成功，地址=%s，端口=%s", ip, port);

    // listen 的作用是把这个 socket 变成监听 socket。
    // 第二个参数 10 表示等待队列上限。
    ret=listen(*fd,10);
    ERROR_CHECK(ret,-1,"启动监听");
    LOG_INFO("服务端开始监听，描述符=%d", *fd);
}
