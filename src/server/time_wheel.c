#include <stdlib.h>
#include <string.h>
#include "time_wheel.h"

/**
 * @brief  把一个 fd 节点追加到指定槽位尾部
 * @param  slot 时间轮槽位
 * @param  fd 目标 fd
 * @param  expire_tick 该节点对应的过期 tick
 * @return 成功返回 0，失败返回 -1
 */
static int append_wheel_node(TimeWheelSlot *slot, int fd, unsigned long long expire_tick) {
    TimeWheelNode *node = NULL;

    // 第一步：为新的时间轮节点申请空间。
    node = (TimeWheelNode *)calloc(1, sizeof(TimeWheelNode));
    if (node == NULL) {
        return -1;
    }

    // 第二步：记录该节点对应的 fd 和过期 tick。
    node->fd = fd;
    node->expire_tick = expire_tick;

    // 第三步：把节点追加到槽位尾部。
    // 槽位为空时，新节点同时作为头节点和尾节点。
    if (slot->head == NULL) {
        slot->head = node;
        slot->tail = node;
        return 0;
    }

    // 槽位非空时，尾插到链表末尾。
    slot->tail->next = node;
    slot->tail = node;
    return 0;
}

/**
 * @brief  初始化时间轮
 * @param  wheel 时间轮对象
 * @param  slot_count 时间轮槽位数量
 * @param  timeout_seconds 连接超时秒数
 * @return 成功返回 0，失败返回 -1
 */
int time_wheel_init(TimeWheel *wheel, int slot_count, int timeout_seconds) {
    // 第一步：校验时间轮参数。
    if (wheel == NULL || slot_count <= 0 || timeout_seconds <= 0) {
        return -1;
    }

    // 第二步：先整体清零，确保结构体处于已知状态。
    memset(wheel, 0, sizeof(TimeWheel));

    // 第三步：为全部槽位申请数组空间。
    wheel->slots = (TimeWheelSlot *)calloc((size_t)slot_count, sizeof(TimeWheelSlot));
    if (wheel->slots == NULL) {
        return -1;
    }

    // 第四步：记录槽位数量、超时时间和当前 tick。
    wheel->slot_count = slot_count;
    wheel->timeout_seconds = timeout_seconds;
    wheel->current_tick = 0;
    return 0;
}

/**
 * @brief  释放时间轮中的所有动态资源
 * @param  wheel 时间轮对象
 * @return 无
 */
void time_wheel_destroy(TimeWheel *wheel) {
    int idx = 0;

    // 第一步：时间轮未初始化时，无需继续释放。
    if (wheel == NULL || wheel->slots == NULL) {
        return;
    }

    // 第二步：逐个槽位释放内部链表节点。
    for (idx = 0; idx < wheel->slot_count; ++idx) {
        TimeWheelNode *cur = wheel->slots[idx].head;
        while (cur != NULL) {
            TimeWheelNode *next = cur->next;
            free(cur);
            cur = next;
        }
    }

    // 第三步：释放槽位数组，并把结构体恢复为初始状态。
    free(wheel->slots);
    wheel->slots = NULL;
    wheel->slot_count = 0;
    wheel->timeout_seconds = 0;
    wheel->current_tick = 0;
}

/**
 * @brief  刷新一个连接的超时位置
 * @param  wheel 时间轮对象
 * @param  state 当前连接状态
 * @return 成功返回新的过期 tick，失败返回 0
 */
unsigned long long time_wheel_refresh(TimeWheel *wheel, ServerConnState *state) {
    unsigned long long expire_tick = 0;
    int slot_index = 0;

    // 第一步：校验输入参数。
    if (wheel == NULL || wheel->slots == NULL || state == NULL) {
        return 0;
    }

    // 第二步：根据当前 tick 和超时时间计算新的过期 tick。
    expire_tick = wheel->current_tick + (unsigned long long)wheel->timeout_seconds;

    // 第三步：根据过期 tick 计算目标槽位下标。
    slot_index = (int)(expire_tick % (unsigned long long)wheel->slot_count);

    // 第四步：把当前连接挂到目标槽位尾部。
    if (append_wheel_node(&wheel->slots[slot_index], state->fd, expire_tick) != 0) {
        return 0;
    }

    // 第五步：同步更新连接状态中的时间轮元数据。
    state->expire_tick = expire_tick;
    state->wheel_slot = slot_index;
    conn_state_touch(state);
    return expire_tick;
}

/**
 * @brief  推进时间轮一格，并收集本轮真正超时的 fd
 * @param  wheel 时间轮对象
 * @param  manager 连接状态管理器
 * @param  expired_fds 输出数组，用来保存超时 fd
 * @param  max_expired expired_fds 的最大容量
 * @return 本轮收集到的超时 fd 个数
 */
int time_wheel_tick(TimeWheel *wheel, ConnManager *manager, int *expired_fds, int max_expired) {
    int expired_count = 0;
    int slot_index = 0;
    TimeWheelNode *cur = NULL;

    // 第一步：校验输入参数。
    if (wheel == NULL || wheel->slots == NULL || manager == NULL || expired_fds == NULL || max_expired <= 0) {
        return 0;
    }

    // 第二步：推进当前 tick，并定位本轮需要检查的槽位。
    wheel->current_tick++;
    slot_index = (int)(wheel->current_tick % (unsigned long long)wheel->slot_count);
    cur = wheel->slots[slot_index].head;

    // 第三步：遍历该槽位中的全部节点。
    // 只有“当前连接状态仍存在，且 expire_tick 与节点记录一致”的 fd，
    // 才说明它确实在本轮到期。
    while (cur != NULL) {
        ServerConnState *state = conn_manager_get(manager, cur->fd);

        if (state != NULL &&
            state->fd == cur->fd &&
            state->expire_tick == cur->expire_tick &&
            expired_count < max_expired) {
            expired_fds[expired_count++] = cur->fd;
        }

        // 第四步：当前槽位已经完成检查，可以释放节点。
        TimeWheelNode *next = cur->next;
        free(cur);
        cur = next;
    }

    // 第五步：把当前槽位恢复为空状态。
    wheel->slots[slot_index].head = NULL;
    wheel->slots[slot_index].tail = NULL;
    return expired_count;
}
