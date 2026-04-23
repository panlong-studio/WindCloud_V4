#ifndef CONTROL_CONN_H
#define CONTROL_CONN_H

#include "protocol.h"

struct TimeWheelNode;

// 控制连接状态结构体。
// 服务端主线程会为每条控制连接维护一份。
typedef struct ControlConn {
    int fd;                               // 控制连接 fd
    ClientContext ctx;                    // 当前控制连接的会话上下文
    long long expire_tick;                // 当前连接预计超时的时刻
    int wheel_slot;                       // 当前所在的时间轮槽位
    struct TimeWheelNode *wheel_node;     // 时间轮节点指针
} ControlConn;

/**
 * @brief  初始化控制连接状态结构体
 * @param  conn 控制连接状态结构体地址
 * @param  fd 控制连接 fd
 * @return 无
 */
void control_conn_init(ControlConn *conn, int fd);

#endif
