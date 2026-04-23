#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_



#include "queue.h"
#include <pthread.h>

typedef struct thread_pool thread_pool_t;

/*
 * 每个工作线程都会拿到一个自己的编号。
 * 这样线程开始处理客户端时，就能把“我当前正在处理哪个 fd”
 * 记录到线程池里，退出时主线程就能找到这些 fd 并主动 shutdown。
 */
typedef struct worker_arg{
    thread_pool_t *pool;
    int index;
}worker_arg_t;

typedef struct thread_pool{
    int num;
    pthread_t* thread_id_arr;
    worker_arg_t *worker_arg_arr;
    queue_t queue;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int exitFlag;
    int *busy_fds;
}thread_pool_t;

void init_thread_pool(thread_pool_t* pool,int num);
void destroy_thread_pool(thread_pool_t *pool);

#endif
