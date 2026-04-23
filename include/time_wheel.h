#ifndef TIME_WHEEL_H
#define TIME_WHEEL_H

#define TIME_WHEEL_SLOT_COUNT 60

typedef struct ControlConn ControlConn;

// 时间轮节点结构体。
// 每个控制连接在时间轮中对应一个节点。
typedef struct TimeWheelNode {
    struct TimeWheelNode *prev;
    struct TimeWheelNode *next;
    ControlConn *conn;
} TimeWheelNode;

// 时间轮结构体。
// 服务端主线程每秒推进一次。
typedef struct {
    TimeWheelNode *slots[TIME_WHEEL_SLOT_COUNT];
    long long current_tick;
    int timeout_sec;
} TimeWheel;

typedef void (*time_wheel_expire_cb)(ControlConn *conn, void *arg);

/**
 * @brief  初始化时间轮
 * @param  wheel 时间轮结构体地址
 * @param  timeout_sec 超时秒数
 * @return 无
 */
void time_wheel_init(TimeWheel *wheel, int timeout_sec);

/**
 * @brief  将控制连接加入时间轮
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
void time_wheel_add(TimeWheel *wheel, ControlConn *conn);

/**
 * @brief  刷新控制连接在时间轮中的超时位置
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
void time_wheel_refresh(TimeWheel *wheel, ControlConn *conn);

/**
 * @brief  将控制连接从时间轮中移除
 * @param  wheel 时间轮结构体地址
 * @param  conn 控制连接结构体地址
 * @return 无
 */
void time_wheel_remove(TimeWheel *wheel, ControlConn *conn);

/**
 * @brief  推进时间轮一格，并处理当前槽位中的超时连接
 * @param  wheel 时间轮结构体地址
 * @param  cb 超时回调函数
 * @param  arg 回调函数的透传参数
 * @return 无
 */
void time_wheel_tick(TimeWheel *wheel, time_wheel_expire_cb cb, void *arg);

#endif
