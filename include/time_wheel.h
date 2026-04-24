#ifndef _TIME_WHEEL_H_
#define _TIME_WHEEL_H_

#include "conn_manager.h"

/* 单个时间轮结点，对应一个连接的一次超时记录。 */
typedef struct TimeWheelNode {
    int fd;                           /* 这条记录对应的连接 fd */
    unsigned long long expire_tick;   /* 该连接本次记录对应的过期 tick */
    struct TimeWheelNode *next;       /* 同一槽位链表中的下一个结点 */
} TimeWheelNode;

/* 单个槽位中的链表头尾指针。 */
typedef struct {
    TimeWheelNode *head;  /* 当前槽位链表头指针 */
    TimeWheelNode *tail;  /* 当前槽位链表尾指针 */
} TimeWheelSlot;

/* 简单循环时间轮结构。 */
typedef struct {
    int slot_count;                    /* 时间轮总槽位数 */
    int timeout_seconds;               /* 连接超时秒数 */
    unsigned long long current_tick;   /* 当前已经推进到的 tick */
    TimeWheelSlot *slots;              /* 全部槽位数组 */
} TimeWheel;

/**
 * @brief  初始化时间轮
 * @param  wheel 时间轮对象
 * @param  slot_count 时间轮槽位数量
 * @param  timeout_seconds 连接超时秒数
 * @return 成功返回 0，失败返回 -1
 */
int time_wheel_init(TimeWheel *wheel, int slot_count, int timeout_seconds);

/**
 * @brief  释放时间轮中的所有动态资源
 * @param  wheel 时间轮对象
 * @return 无
 */
void time_wheel_destroy(TimeWheel *wheel);

/**
 * @brief  刷新一个连接的超时位置
 * @param  wheel 时间轮对象
 * @param  state 当前连接状态
 * @return 成功返回新的过期 tick，失败返回 0
 */
unsigned long long time_wheel_refresh(TimeWheel *wheel, ServerConnState *state);

/**
 * @brief  推进时间轮一格，并收集本轮真正超时的 fd
 * @param  wheel 时间轮对象
 * @param  manager 连接状态管理器
 * @param  expired_fds 输出数组，用来保存超时 fd
 * @param  max_expired expired_fds 的最大容量
 * @return 本轮收集到的超时 fd 个数
 */
int time_wheel_tick(TimeWheel *wheel, ConnManager *manager, int *expired_fds, int max_expired);

#endif
