#ifndef _QUEUE_H_
#define _QUEUE_H_

#include "protocol.h"

/* 传输任务由主线程构造，工作线程按任务内容执行一次上传或下载。 */
typedef struct{
    int client_fd;                  /* 本次传输连接对应的客户端 fd */
    int cmd_type;                   /* 任务类型，当前只会是 puts 或 gets */
    ClientContext ctx;              /* 执行任务时需要恢复出的用户上下文 */
    char arg[CMD_DATA_LEN];         /* 上传或下载目标名称 */
}transfer_task_t;

/* 队列结点中保存一条完整传输任务。 */
typedef struct node_s{
    transfer_task_t task;           /* 当前结点保存的传输任务 */
    struct node_s*pNext;            /* 指向下一个队列结点 */
}node_t;

/* 简单链式队列，线程池通过它保存待处理任务。 */
typedef struct queue_s{
    node_t* head;                   /* 队头结点，出队时从这里取任务 */
    node_t* end;                    /* 队尾结点，入队时追加到这里 */
    int size;                       /* 当前队列中等待处理的任务数量 */
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
