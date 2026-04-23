#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "worker.h"
#include "thread_pool.h"
#include "queue.h"
#include "log.h"
#include "protocol.h"
#include "session.h"

/**
 * @brief  线程池里的工作线程入口函数
 * @param  arg 实际上传入的是 worker_arg_t*，里面保存线程池指针和线程编号
 * @return 线程退出时返回 NULL
 */
void* thread_func(void *arg) {
    /* 第一步：还原线程启动参数。 */
    worker_arg_t *worker_arg = (worker_arg_t *)arg;
    thread_pool_t *pool = worker_arg->pool;
    int worker_index = worker_arg->index;
    
    /* 第二步：持续处理队列中的传输任务，直到线程池准备退出。 */
    while (1) {
        /* 访问任务队列前先加锁。 */
        pthread_mutex_lock(&pool->lock);

        /* 队列为空时，工作线程在条件变量上等待。 */
        while (pool->queue.size==0 && !pool->exitFlag) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        /* 线程池退出时，当前线程结束循环。 */
        if (pool->exitFlag) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        /* 从队列头部取出一条待处理任务。 */
        transfer_task_t task;
        if (deQueue(&pool->queue, &task) != 0) {
            pthread_mutex_unlock(&pool->lock);
            continue;
        }

        /* 记录当前线程正在处理的连接，便于服务端退出时统一回收。 */
        pool->busy_fds[worker_index] = task.client_fd;

        /* 任务已经出队，后续执行阶段不再访问队列，可以释放锁。 */
        pthread_mutex_unlock(&pool->lock);

        LOG_INFO("工作线程开始处理传输任务，线程=%lu，客户端fd=%d，命令类型=%d",
                 (unsigned long)pthread_self(),
                 task.client_fd,
                 task.cmd_type);

        /* 第三步：执行本次上传或下载任务。 */
        session_handle_transfer_task(&task);

        /* 第四步：任务结束后关闭本次传输连接。 */
        shutdown(task.client_fd, SHUT_RDWR);
        close(task.client_fd);

        /* 第五步：清理忙碌记录，恢复线程空闲状态。 */
        pthread_mutex_lock(&pool->lock);
        pool->busy_fds[worker_index] = -1;
        pthread_mutex_unlock(&pool->lock);

        LOG_INFO("工作线程处理完成，线程=%lu，客户端fd=%d", (unsigned long)pthread_self(), task.client_fd);
    }

    /* 第六步：线程退出前记录日志。 */
    LOG_INFO("工作线程退出，线程=%lu", (unsigned long)pthread_self());
    return NULL;
}
