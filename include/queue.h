#ifndef _QUEUE_H_
#define _QUEUE_H_

#include "protocol.h"

/* 传输任务由主线程构造，工作线程按任务内容执行一次上传或下载。 */
typedef struct{
    int client_fd;
    int cmd_type;
    ClientContext ctx;
    char arg[CMD_DATA_LEN];
}transfer_task_t;

/* 队列结点中保存一条完整传输任务。 */
typedef struct node_s{
    transfer_task_t task;
    struct node_s*pNext;
}node_t;

/* 简单链式队列，线程池通过它保存待处理任务。 */
typedef struct queue_s{
    node_t* head;
    node_t* end;
    int size;
}queue_t;

/**
 * @brief  把一条传输任务放入队列尾部
 * @param  pQueue 任务队列
 * @param  task 要入队的任务
 * @return 成功返回 0，失败返回 -1
 */
int enQueue(queue_t*pQueue,const transfer_task_t *task);

/**
 * @brief  从队列头部取出一条传输任务
 * @param  pQueue 任务队列
 * @param  task 输出参数，用来保存出队任务
 * @return 成功返回 0，失败返回 -1
 */
int deQueue(queue_t*pQueue,transfer_task_t *task);

#endif
