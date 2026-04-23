#ifndef _CONN_MANAGER_H_
#define _CONN_MANAGER_H_

#include <time.h>
#include "protocol.h"

typedef struct ServerConnState {
    int fd;
    int is_logged_in;
    int user_id;
    int current_dir_id;
    int wheel_slot;
    unsigned long long expire_tick;
    time_t last_active_time;
    char current_path[CMD_DATA_LEN];
    char token[TOKEN_LEN];
    struct ServerConnState *next;
} ServerConnState;

typedef struct {
    ServerConnState *head;
} ConnManager;

/**
 * @brief  初始化连接状态管理器
 * @param  manager 连接状态管理器
 * @return 无
 */
void conn_manager_init(ConnManager *manager);

/**
 * @brief  销毁连接状态管理器中的所有状态节点
 * @param  manager 连接状态管理器
 * @return 无
 */
void conn_manager_destroy(ConnManager *manager);

/**
 * @brief  为一个新连接创建默认状态
 * @param  manager 连接状态管理器
 * @param  fd 新连接的文件描述符
 * @return 成功返回状态节点指针，失败返回 NULL
 */
ServerConnState *conn_manager_add(ConnManager *manager, int fd);

/**
 * @brief  根据 fd 查找连接状态
 * @param  manager 连接状态管理器
 * @param  fd 目标连接 fd
 * @return 找到返回状态节点指针，找不到返回 NULL
 */
ServerConnState *conn_manager_get(ConnManager *manager, int fd);

/**
 * @brief  删除指定 fd 对应的连接状态
 * @param  manager 连接状态管理器
 * @param  fd 要删除的连接 fd
 * @return 成功返回 0，失败返回 -1
 */
int conn_manager_remove(ConnManager *manager, int fd);

/**
 * @brief  把一个服务端连接状态转换成 ClientContext
 * @param  state 当前连接状态
 * @param  ctx 输出参数，用来保存会话上下文
 * @return 成功返回 0，失败返回 -1
 */
int conn_state_to_client_ctx(ServerConnState *state, ClientContext *ctx);

/**
 * @brief  把 ClientContext 中的目录状态同步回连接状态
 * @param  state 当前连接状态
 * @param  ctx 会话上下文
 * @return 成功返回 0，失败返回 -1
 */
int conn_state_sync_from_client_ctx(ServerConnState *state, const ClientContext *ctx);

/**
 * @brief  更新连接的最后活跃时间
 * @param  state 当前连接状态
 * @return 无
 */
void conn_state_touch(ServerConnState *state);

/**
 * @brief  把连接状态重置为“未登录”的默认状态
 * @param  state 当前连接状态
 * @return 无
 */
void conn_state_reset(ServerConnState *state);

#endif
