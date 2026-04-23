#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "thread_pool.h"
#include "worker.h"
#include "error_check.h"
#include "log.h"


/**
 * @brief  初始化线程池并创建所有工作线程
 * @param  pool 线程池结构体地址
 * @param  num 工作线程数量
 * @return 无
 */
void init_thread_pool(thread_pool_t* pool,int num){
    // exitFlag=0 表示线程池当前处于正常工作状态。
    pool->exitFlag=0;

    // 记录线程总数，后面 join 和遍历时要用。
    pool->num=num;

    // 初始化互斥锁，用来保护任务队列和 busy_fds。
    pthread_mutex_init(&pool->lock,NULL);

    // 初始化条件变量，队列为空时，线程会睡在这里。
    pthread_cond_init(&pool->cond,NULL);

    // 先把任务队列整体清零，表示初始时队列为空。
    memset(&pool->queue,0,sizeof(queue_t));

    // 为线程 ID 数组分配空间。
    // 后面 destroy_thread_pool 会负责统一释放。
    pool->thread_id_arr=(pthread_t*)malloc(num*sizeof(pthread_t));

    // 为每个线程准备一个 worker_arg_t。
    // 这样每个线程都能知道“自己是第几个线程”。
    pool->worker_arg_arr=(worker_arg_t*)malloc(num*sizeof(worker_arg_t));

    // busy_fds 用来记录每个线程当前正在处理哪个 client_fd。
    pool->busy_fds=(int*)malloc(num*sizeof(int));

    // 只要有一个 malloc 失败，就直接退出。
    if(pool->thread_id_arr==NULL || pool->worker_arg_arr==NULL || pool->busy_fds==NULL){
        LOG_ERROR("线程池内存分配失败");
        exit(1);
    }

    // 初始状态下，没有线程正在处理客户端，所以全部设成 -1。
    // 这个数组主要用于服务端退出时，定位每个工作线程当前正忙着处理哪个客户端。
    for(int idx=0;idx<num;++idx){
        pool->busy_fds[idx]=-1;
    }

    // 真正开始创建工作线程。
    for(int idx=0;idx<num;++idx){
        // 告诉当前线程：线程池是谁。
        pool->worker_arg_arr[idx].pool=pool;

        // 告诉当前线程：你自己的编号是多少。
        pool->worker_arg_arr[idx].index=idx;

        // pthread_create 成功返回 0，失败返回正的错误码。
        // 所以这里不能再按 -1 判断。
        int ret=pthread_create(&pool->thread_id_arr[idx],NULL,thread_func,(void*)&pool->worker_arg_arr[idx]);
        THREAD_ERROR_CHECK(ret,"创建工作线程");
    }

    LOG_INFO("线程池初始化完成，工作线程数=%d", num);

}

/**
 * @brief  销毁线程池申请的动态资源
 * @param  pool 线程池结构体地址
 * @return 无
 */
void destroy_thread_pool(thread_pool_t *pool){
    // 先做空指针保护。
    if(pool==NULL){
        return;
    }

    // 下面 3 个 free 对应 init_thread_pool 里的 3 次 malloc。
    free(pool->thread_id_arr);
    free(pool->worker_arg_arr);
    free(pool->busy_fds);

    // 最后销毁同步工具。
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
}
