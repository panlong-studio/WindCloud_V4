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
#include "file_cmds.h"
#include "file_transfer.h"
#include "path_utils.h"
#include "sha256_utils.h"

/**
 * @brief  线程池里的工作线程入口函数
 * @param  arg 实际上传入的是 worker_arg_t*，里面保存线程池指针和线程编号
 * @return 线程退出时返回 NULL
 */
void* thread_func(void *arg) {
    // 先把 void* 转回真实类型。
    worker_arg_t *worker_arg = (worker_arg_t *)arg;

    // 取出所属的线程池。
    thread_pool_t *pool = worker_arg->pool;

    // 取出当前线程自己的编号。
    int worker_index = worker_arg->index;
    
    // 工作线程会一直循环，直到收到退出信号。
    while (1) {
        // 先加锁，准备安全访问任务队列。
        pthread_mutex_lock(&pool->lock);

        // 如果队列为空，而且线程池也没有退出，
        // 那当前线程就睡到条件变量上，等待主线程派发新任务。
        while (pool->queue.size==0 && !pool->exitFlag) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        // 被唤醒后，先看是不是因为退出而醒来。
        if (pool->exitFlag) {
            // 退出前别忘了解锁。
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        // 走到这里说明队列里真的有任务。
        // 从队列头部取出一个客户端 fd。
        int client_fd = deQueue(&pool->queue);

        // 记录“当前线程正在处理哪个 fd”。
        // 这样主线程退出时，才能找到这些 fd 并主动 shutdown。
        pool->busy_fds[worker_index] = client_fd;

        // 取到任务后就可以解锁了，
        // 因为后面的 handle_request 不再操作任务队列。
        pthread_mutex_unlock(&pool->lock);

        // 打印调试信息，方便观察哪个线程在工作。
        LOG_INFO("工作线程开始处理客户端，线程=%lu，客户端fd=%d", (unsigned long)pthread_self(), client_fd);

        // 真正处理这个客户端的所有命令。
        handle_request(client_fd);  // 你的处理函数

        // 处理完后，主动 shutdown 一次，确保连接进入关闭流程。
        shutdown(client_fd, SHUT_RDWR);

        // 再关闭文件描述符，回收内核资源。
        close(client_fd);

        // 准备修改 busy_fds，所以重新加锁。
        pthread_mutex_lock(&pool->lock);

        // 当前线程已经空闲了，把记录改回 -1。
        pool->busy_fds[worker_index] = -1;
        pthread_mutex_unlock(&pool->lock);

        // 打印线程处理完成的信息。
        LOG_INFO("工作线程处理完成，线程=%lu，客户端fd=%d", (unsigned long)pthread_self(), client_fd);
    }

    LOG_INFO("工作线程退出，线程=%lu", (unsigned long)pthread_self());
    return NULL;
}
