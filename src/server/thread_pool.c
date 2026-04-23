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
    /* 第一步：初始化线程池的基础状态。 */
    pool->exitFlag=0;
    pool->num=num;

    /* 第二步：初始化同步工具，保护任务队列和忙碌线程记录。 */
    pthread_mutex_init(&pool->lock,NULL);
    pthread_cond_init(&pool->cond,NULL);

    /* 第三步：把任务队列清零，表示线程池刚启动时没有待处理任务。 */
    memset(&pool->queue,0,sizeof(queue_t));

    /* 第四步：申请线程池运行所需的动态数组。 */
    pool->thread_id_arr=(pthread_t*)malloc(num*sizeof(pthread_t));
    pool->worker_arg_arr=(worker_arg_t*)malloc(num*sizeof(worker_arg_t));
    pool->busy_fds=(int*)malloc(num*sizeof(int));

    /* 任意一块关键内存申请失败时，线程池都无法继续工作。 */
    if(pool->thread_id_arr==NULL || pool->worker_arg_arr==NULL || pool->busy_fds==NULL){
        LOG_ERROR("线程池内存分配失败");
        exit(1);
    }

    /* 第五步：初始化忙碌连接记录，-1 表示当前线程空闲。 */
    for(int idx=0;idx<num;++idx){
        pool->busy_fds[idx]=-1;
    }

    /* 第六步：依次创建所有工作线程。 */
    for(int idx=0;idx<num;++idx){
        pool->worker_arg_arr[idx].pool=pool;
        pool->worker_arg_arr[idx].index=idx;

        /* 每个线程都通过 worker_arg_t 获取线程池地址和线程编号。 */
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
    /* 先做空指针保护。 */
    if(pool==NULL){
        return;
    }

    /* 释放初始化阶段申请的动态数组。 */
    free(pool->thread_id_arr);
    free(pool->worker_arg_arr);
    free(pool->busy_fds);

    /* 最后销毁互斥锁和条件变量。 */
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
}
