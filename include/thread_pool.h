#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_

#include "queue.h"
#include <pthread.h>

typedef struct thread_pool thread_pool_t;

/* 工作线程启动时携带的参数。 */
typedef struct worker_arg{
    thread_pool_t *pool;            /* 当前线程所属的线程池 */
    int index;                      /* 当前线程在数组中的下标 */
}worker_arg_t;

/* 线程池负责保存工作线程、任务队列以及同步资源。 */
typedef struct thread_pool{
    int num;                        /* 线程池中的工作线程数量 */
    pthread_t* thread_id_arr;       /* 保存每个工作线程的线程 ID */
    worker_arg_t *worker_arg_arr;   /* 保存每个工作线程启动参数 */
    queue_t queue;                  /* 等待工作线程处理的任务队列 */
    pthread_mutex_t lock;           /* 保护任务队列和退出标记的互斥锁 */
    pthread_cond_t cond;            /* 队列为空时工作线程等待的条件变量 */
    int exitFlag;                   /* 主线程通知工作线程退出的标记 */
    int *busy_fds;                  /* 记录每个工作线程当前处理的客户端 fd */
}thread_pool_t;

/**
 * @brief  初始化线程池，创建工作线程并准备同步资源
 * @param  pool 线程池对象
 * @param  num 需要创建的工作线程数量
 * @return 无
 */
void init_thread_pool(thread_pool_t* pool,int num);

/**
 * @brief  销毁线程池，回收线程数组和同步资源
 * @param  pool 线程池对象
 * @return 无
 */
void destroy_thread_pool(thread_pool_t *pool);

#endif
