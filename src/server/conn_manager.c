#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "conn_manager.h"

/**
 * @brief  初始化连接状态管理器
 * @param  manager 连接状态管理器
 * @return 无
 */
void conn_manager_init(ConnManager *manager) {
    // 第一步：先做空指针保护，避免调用方传入空地址时发生非法访问。
    if (manager == NULL) {
        return;
    }

    // 第二步：初始化链表头指针。
    // 连接状态管理器本质上维护的是一条单链表，
    // 初始化时链表中还没有任何连接状态节点。
    manager->head = NULL;
}

/**
 * @brief  销毁连接状态管理器中的所有状态节点
 * @param  manager 连接状态管理器
 * @return 无
 */
void conn_manager_destroy(ConnManager *manager) {
    ServerConnState *cur = NULL;
    ServerConnState *next = NULL;

    // 第一步：先判断管理器是否有效。
    if (manager == NULL) {
        return;
    }

    // 第二步：从链表头开始依次释放每一个连接状态节点。
    // 这里先保存 next，再释放当前节点，避免链表断开后无法继续遍历。
    cur = manager->head;
    while (cur != NULL) {
        next = cur->next;
        free(cur);
        cur = next;
    }

    // 第三步：链表全部释放后，把头指针恢复为 NULL。
    manager->head = NULL;
}

/**
 * @brief  把连接状态重置为默认的未登录状态
 * @param  state 当前连接状态
 * @return 无
 */
void conn_state_reset(ServerConnState *state) {
    // 第一步：先做空指针保护。
    if (state == NULL) {
        return;
    }

    // 第二步：把连接状态恢复到“新连接、未登录”的默认值。
    // 这些字段在 accept 新连接后，或者连接状态需要重置时都会被重新填写。
    state->is_logged_in = 0;
    state->user_id = -1;
    state->current_dir_id = 0;
    state->wheel_slot = -1;
    state->expire_tick = 0;
    state->last_active_time = time(NULL);

    // 第三步：当前路径恢复为根目录。
    strcpy(state->current_path, "/");

    // 第四步：清空 token 字符串。
    state->token[0] = '\0';
}

/**
 * @brief  为一个新连接创建默认状态
 * @param  manager 连接状态管理器
 * @param  fd 新连接的文件描述符
 * @return 成功返回状态节点指针，失败返回 NULL
 */
ServerConnState *conn_manager_add(ConnManager *manager, int fd) {
    ServerConnState *node = NULL;

    // 第一步：先确认管理器有效。
    if (manager == NULL) {
        return NULL;
    }

    // 第二步：为新的连接状态申请节点空间。
    node = (ServerConnState *)calloc(1, sizeof(ServerConnState));
    if (node == NULL) {
        return NULL;
    }

    // 第三步：记录该状态对应的客户端 fd。
    node->fd = fd;

    // 第四步：把其余状态恢复为默认值。
    conn_state_reset(node);

    // 第五步：采用头插法把新节点加入链表。
    // 当前项目中的连接数量不大，头插法实现最简单，查找逻辑也足够清晰。
    node->next = manager->head;
    manager->head = node;
    return node;
}

/**
 * @brief  根据 fd 查找连接状态
 * @param  manager 连接状态管理器
 * @param  fd 目标连接 fd
 * @return 找到返回状态节点指针，找不到返回 NULL
 */
ServerConnState *conn_manager_get(ConnManager *manager, int fd) {
    ServerConnState *cur = NULL;

    // 第一步：先确认管理器有效。
    if (manager == NULL) {
        return NULL;
    }

    // 第二步：从链表头开始顺序查找目标 fd。
    // 找到后立即返回对应状态节点。
    cur = manager->head;
    while (cur != NULL) {
        if (cur->fd == fd) {
            return cur;
        }
        cur = cur->next;
    }

    return NULL;
}

/**
 * @brief  删除指定 fd 对应的连接状态
 * @param  manager 连接状态管理器
 * @param  fd 要删除的连接 fd
 * @return 成功返回 0，失败返回 -1
 */
int conn_manager_remove(ConnManager *manager, int fd) {
    ServerConnState *cur = NULL;
    ServerConnState *prev = NULL;

    // 第一步：先确认管理器有效。
    if (manager == NULL) {
        return -1;
    }

    // 第二步：顺序查找目标节点，同时保留前驱指针。
    // 删除单链表节点时，需要区分“删除头节点”和“删除普通节点”这两种情况。
    cur = manager->head;
    while (cur != NULL) {
        if (cur->fd == fd) {
            if (prev == NULL) {
                manager->head = cur->next;
            } else {
                prev->next = cur->next;
            }

            // 第三步：释放目标节点并返回成功。
            free(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }

    return -1;
}

/**
 * @brief  把一个服务端连接状态转换成 ClientContext
 * @param  state 当前连接状态
 * @param  ctx 输出参数，用来保存会话上下文
 * @return 成功返回 0，失败返回 -1
 */
int conn_state_to_client_ctx(ServerConnState *state, ClientContext *ctx) {
    // 第一步：先校验输入参数。
    if (state == NULL || ctx == NULL) {
        return -1;
    }

    // 第二步：先整体清零，避免目标结构体中残留旧数据。
    memset(ctx, 0, sizeof(ClientContext));

    // 第三步：把连接状态中的关键会话信息复制到 ClientContext。
    // 后续 file_cmds 和 file_transfer 模块都依赖这个结构体继续工作。
    ctx->user_id = state->user_id;
    ctx->current_dir_id = state->current_dir_id;
    strncpy(ctx->current_path, state->current_path, sizeof(ctx->current_path) - 1);
    return 0;
}

/**
 * @brief  把 ClientContext 中的目录状态同步回连接状态
 * @param  state 当前连接状态
 * @param  ctx 会话上下文
 * @return 成功返回 0，失败返回 -1
 */
int conn_state_sync_from_client_ctx(ServerConnState *state, const ClientContext *ctx) {
    // 第一步：先校验输入参数。
    if (state == NULL || ctx == NULL) {
        return -1;
    }

    // 第二步：把业务处理后的会话结果回写到连接状态中。
    // 这样主线程维护的连接状态与业务层使用的 ClientContext 就保持一致。
    state->user_id = ctx->user_id;
    state->current_dir_id = ctx->current_dir_id;
    state->is_logged_in = (ctx->user_id != -1);
    strncpy(state->current_path, ctx->current_path, sizeof(state->current_path) - 1);
    state->current_path[sizeof(state->current_path) - 1] = '\0';
    return 0;
}

/**
 * @brief  更新连接的最后活跃时间
 * @param  state 当前连接状态
 * @return 无
 */
void conn_state_touch(ServerConnState *state) {
    // 更新时间前先确认状态节点有效。
    if (state == NULL) {
        return;
    }

    // 记录最近一次活动时间，供超时控制逻辑参考。
    state->last_active_time = time(NULL);
}
