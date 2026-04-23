#ifndef _CONN_MANAGER_H_
#define _CONN_MANAGER_H_

#include <time.h>
#include "protocol.h"

/* 单个主连接在服务端维护的状态信息。 */
typedef struct ServerConnState {
    int fd;                             /* 当前连接 fd */
    int is_logged_in;                   /* 是否已登录 */
    int user_id;                        /* 当前连接对应的用户 ID */
    int current_dir_id;                 /* 当前目录节点 ID */
    int wheel_slot;                     /* 当前挂入的时间轮槽位 */
    unsigned long long expire_tick;     /* 当前记录的过期 tick */
    time_t last_active_time;            /* 最近一次活跃时间 */
    char current_path[CMD_DATA_LEN];    /* 当前虚拟路径 */
    char token[TOKEN_LEN];              /* 当前连接保存的 token */
    struct ServerConnState *next;       /* 单链表后继指针 */
} ServerConnState;

/* 连接状态管理器，当前版本使用单链表保存所有主连接状态。 */
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
