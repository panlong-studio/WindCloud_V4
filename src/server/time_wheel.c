#include <stdlib.h>
#include <string.h>
#include "time_wheel.h"
#include "control_conn.h"

/**
 * @brief  从当前槽位链表中断开一个节点
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
static void detach_node(TimeWheel *wheel, ControlConn *conn) {
    TimeWheelNode *node = conn->wheel_node;

    if (node == NULL) {
        return;
    }

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        wheel->slots[conn->wheel_slot] = node->next;
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    }

    node->prev = NULL;
    node->next = NULL;
}

/**
 * @brief  初始化时间轮
 * @param  wheel 时间轮结构体地址
 * @param  timeout_sec 超时秒数
 * @return 无
 */
void time_wheel_init(TimeWheel *wheel, int timeout_sec) {
    memset(wheel, 0, sizeof(TimeWheel));
    wheel->timeout_sec = timeout_sec;
    wheel->current_tick = 0;
}

/**
 * @brief  将控制连接加入时间轮
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
void time_wheel_add(TimeWheel *wheel, ControlConn *conn) {
    int slot = 0;
    TimeWheelNode *node = NULL;

    if (conn->wheel_node == NULL) {
        node = (TimeWheelNode *)calloc(1, sizeof(TimeWheelNode));
        if (node == NULL) {
            return;
        }
        node->conn = conn;
        conn->wheel_node = node;
    }

    conn->expire_tick = wheel->current_tick + wheel->timeout_sec;
    slot = (int)(conn->expire_tick % TIME_WHEEL_SLOT_COUNT);
    conn->wheel_slot = slot;

    node = conn->wheel_node;
    node->prev = NULL;
    node->next = wheel->slots[slot];
    if (wheel->slots[slot] != NULL) {
        wheel->slots[slot]->prev = node;
    }
    wheel->slots[slot] = node;
}

/**
 * @brief  刷新控制连接在时间轮中的超时位置
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
void time_wheel_refresh(TimeWheel *wheel, ControlConn *conn) {
    if (conn == NULL) {
        return;
    }

    if (conn->wheel_node != NULL && conn->wheel_slot >= 0) {
        detach_node(wheel, conn);
    }

    time_wheel_add(wheel, conn);
}

/**
 * @brief  将控制连接从时间轮中移除
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
void time_wheel_remove(TimeWheel *wheel, ControlConn *conn) {
    if (conn == NULL || conn->wheel_node == NULL) {
        return;
    }

    detach_node(wheel, conn);
    free(conn->wheel_node);
    conn->wheel_node = NULL;
    conn->wheel_slot = -1;
}

/**
 * @brief  推进时间轮一格，并处理当前槽位中的超时连接
 * @param  wheel 时间轮结构体地址
 * @param  cb 超时回调函数
 * @param  arg 回调函数透传参数
 * @return 无
 */
void time_wheel_tick(TimeWheel *wheel, time_wheel_expire_cb cb, void *arg) {
    int slot = 0;
    TimeWheelNode *node = NULL;
    TimeWheelNode *next = NULL;

    wheel->current_tick += 1;
    slot = (int)(wheel->current_tick % TIME_WHEEL_SLOT_COUNT);
    node = wheel->slots[slot];

    while (node != NULL) {
        next = node->next;
        if (node->conn != NULL && node->conn->expire_tick <= wheel->current_tick) {
            detach_node(wheel, node->conn);
            node->conn->wheel_node = NULL;
            node->conn->wheel_slot = -1;
            if (cb != NULL) {
                cb(node->conn, arg);
            }
            free(node);
        }
        node = next;
    }
}
