#include <string.h>
#include "control_conn.h"

/**
 * @brief  初始化控制连接状态结构体
 * @param  conn 控制连接状态结构体地址
 * @param  fd 控制连接 fd
 * @return 无
 */
void control_conn_init(ControlConn *conn, int fd) {
    memset(conn, 0, sizeof(ControlConn));
    conn->fd = fd;
    conn->ctx.user_id = -1;
    strcpy(conn->ctx.current_path, "/");
    conn->ctx.current_dir_id = 0;
    conn->expire_tick = 0;
    conn->wheel_slot = -1;
    conn->wheel_node = NULL;
}
