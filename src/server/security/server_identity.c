#include <stdio.h>
#include <string.h>
#include "server_identity.h"
#include "config.h"

/**
 * @brief  读取当前服务端自己的 IP 和端口
 * @param  ip 输出参数，用来保存服务端 IP
 * @param  ip_size ip 缓冲区大小
 * @param  port 输出参数，用来保存服务端端口
 * @param  port_size port 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_current_server_address(char *ip, size_t ip_size, char *port, size_t port_size) {
    char tmp_ip[64] = {0};
    char tmp_port[32] = {0};

    if (ip == NULL || ip_size == 0 || port == NULL || port_size == 0) {
        return -1;
    }

    // 这里统一复用配置文件中的 ip 和 port。
    // 这样控制服务器返回的数据源地址，和数据源服务器自我校验使用的是同一套来源。
    // 先从配置文件读当前服务端的监听地址。
    // 如果用户没有配，就回退到项目默认值。
    if (get_target("ip", tmp_ip) != 0) {
        snprintf(tmp_ip, sizeof(tmp_ip), "%s", "127.0.0.1");
    }

    if (get_target("port", tmp_port) != 0) {
        snprintf(tmp_port, sizeof(tmp_port), "%s", "9090");
    }

    // 最后再复制到调用方的缓冲区。
    // 这样调用方不用关心默认值和配置读取细节。
    if (snprintf(ip, ip_size, "%s", tmp_ip) >= (int)ip_size) {
        return -1;
    }

    if (snprintf(port, port_size, "%s", tmp_port) >= (int)port_size) {
        return -1;
    }

    return 0;
}
