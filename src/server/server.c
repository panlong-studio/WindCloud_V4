#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include "queue.h"
#include "thread_pool.h"
#include "worker.h"
#include "epoll.h"
#include "server_socket.h"
#include "config.h"
#include "error_check.h"
#include "log.h"
#include "protocol.h"
#include "session.h"
#include "file_cmds.h"
#include "file_transfer.h"
#include "path_utils.h"
#include "sha256_utils.h"
#include "db_init.h"
#include "db_pool.h"

// pipe_fd[0] 用来读，pipe_fd[1] 用来写。
// 父进程收到 Ctrl+C 后，会往管道里写一个字节。
// 子进程的 epoll 监听到这个字节后，就进入退出流程。
int pipe_fd[2];

/**
 * @brief  SIGINT 信号处理函数，通知子进程退出
 * @param  num 信号编号
 * @return 无
 */
void func(int num){
    (void)num;
    // 往管道里写一个字节，通知子进程该退出了。
    write(pipe_fd[1],"1",1);
}

/**
 * @brief  从配置文件读取指定键，读不到时回退到默认值
 * @param  key 配置项名称
 * @param  value 输出参数，用来保存最终值
 * @param  value_sz value 缓冲区大小
 * @param  default_value 默认值
 * @return 无
 */
static void load_value_or_default(const char *key, char *value, size_t value_sz, const char *default_value) {
    char tmp[256] = {0};

    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    snprintf(value, value_sz, "%s", default_value);
}

/**
 * @brief  初始化日志，并兼容不同启动目录下的日志相对路径
 * @param  level_str 日志级别字符串
 * @param  log_file 原始日志文件路径
 * @return 无
 */
static void init_log_with_fallback(const char *level_str, const char *log_file) {
    const char *real_log_file = log_file;

    // 服务端从根目录或 bin 目录启动时，相对日志路径不同。
    // 这里按真实存在的目录修正日志路径，避免服务端启动早期丢日志。
    if (log_file != NULL && strncmp(log_file, "../", 3) == 0) {
        if (access("../log", F_OK) == 0) {
            real_log_file = log_file;
        } else if (access("./log", F_OK) == 0) {
            real_log_file = log_file + 3;
        }
    }

    if (init_log(level_str, real_log_file) == 0) {
        return;
    }

    init_log(level_str, NULL);
}

/**
 * @brief  服务端主函数，负责初始化配置、数据库、线程池和 epoll 主循环
 * @return 正常结束返回 0，失败返回非 0
 */
int main(){
    // 忽略 SIGPIPE。
    // 这样当对端断开连接后，send 不会直接把进程打死。
    signal(SIGPIPE, SIG_IGN);

    //=====================加载配置========================
    // ip 和 port 用来保存配置文件中的监听地址。
    char ip[64] = {0}; 
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};

    // 先用默认日志路径初始化，确保配置加载阶段的日志也能写入。
    init_log_with_fallback("INFO", "../log/server.log");

    // 加载配置。
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("server_log", log_file, sizeof(log_file), "../log/server.log");

    char db_host[64] = {0};
    char db_user[64] = {0};
    char db_pwd[64] = {0};
    char db_name[64] = {0};

    // 尝试从配置文件读，读不到就用默认值（这里默认用 root 和 密码 123456，供本地测试）
    load_value_or_default("db_host", db_host, sizeof(db_host), "127.0.0.1");
    load_value_or_default("db_user", db_user, sizeof(db_user), "root");
    load_value_or_default("db_pwd",  db_pwd,  sizeof(db_pwd),  "123456"); 
    load_value_or_default("db_name", db_name, sizeof(db_name), "netdisk_db");

    //=================先初始化日志========================
    // 否则 socket/bind/accept 等调用一旦失败，ERROR_CHECK 无法安全打印日志。
    init_log_with_fallback(log_level, log_file);
    LOG_INFO("服务端配置加载完成，地址=%s，端口=%s", ip, port);

    //===============服务端数据库自动建表==========================
    if (init_database(db_host, db_user, db_pwd, db_name) != 0) {
        LOG_ERROR("数据库初始化失败，服务端拒绝启动");
        close_log();
        return 1;
    }

    //=================初始化数据库连接池========================
    if(init_db_pool(db_host, db_user, db_pwd, db_name, 10) != 0) {
        LOG_ERROR("数据库连接池初始化失败，服务端拒绝启动");
        close_log();
        return 1;
    }

    //=================创建管道和子进程========================
    // 创建匿名管道，用于父进程通知子进程退出。
    if (pipe(pipe_fd) != 0) {
        LOG_ERROR("创建管道失败: %s", strerror(errno));
        close_log();
        return 1;
    }
    
    // fork 之后会分成父子两个进程。
    // 父进程专门负责监听 Ctrl+C。
    // 子进程负责真正跑服务器。
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("创建子进程失败: %s", strerror(errno));
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        close_log();
        return 1;
    }

    if(pid != 0){
        // 父进程收到 SIGINT 后，就执行上面的 func。
        signal(SIGINT, func);
        LOG_INFO("服务端父进程等待子进程退出，子进程pid=%d", (int)pid);

        // 父进程等待子进程结束。
        wait(NULL);
        LOG_INFO("服务端父进程退出");
        exit(0);
    }

    // 子进程把自己放进新的进程组，避免和父进程完全绑死在一起。
    if (setpgid(0, 0) != 0) {
        LOG_WARN("设置进程组失败 errno=%d", errno);
    }

    //=================子进程继续执行服务端主逻辑========================

    //-----------创建监听 socket----------------
    // listen_fd 是服务端监听新连接用的 socket。
    int listen_fd = 0;
    init_socket(&listen_fd, ip, port);

    //-----------创建线程池----------------
    thread_pool_t pool;
    init_thread_pool(&pool, 5);

    //-----------创建 epoll 实例----------------
    int epfd = epoll_create(1);
    ERROR_CHECK(epfd, -1, "创建 epoll");

    // 监听 listen_fd：表示有新客户端到来。
    add_epoll_fd(epfd, listen_fd);

    // 监听 pipe_fd[0]：表示父进程通知子进程退出。
    add_epoll_fd(epfd, pipe_fd[0]);
    LOG_INFO("服务端启动成功，地址=%s，端口=%s", ip, port);

    // 主循环，持续等待 epoll 事件。
    while(1){
        // lst 用来保存本轮就绪的事件列表。
        struct epoll_event lst[10];

        // epoll_wait 会阻塞，直到至少有一个 fd 就绪。
        int nready = epoll_wait(epfd, lst, 10, -1);
        if (nready == -1) {
            if (errno == EINTR) {
                continue;
            }
            ERROR_CHECK(nready, -1, "等待 epoll 事件");
        }
        
        // 依次处理本轮所有就绪事件。
        for(int idx = 0; idx < nready; idx++){
            // 取出当前就绪的 fd。
            int fd = lst[idx].data.fd;

            if(fd == pipe_fd[0]){
                // 读走管道中的退出通知字节。
                char buf[10];
                read(fd, buf, sizeof(buf));
                LOG_INFO("服务端收到退出信号");

                // 修改线程池共享数据前，先加锁。
                pthread_mutex_lock(&pool.lock);

                // 置 1 表示线程池进入退出状态。
                pool.exitFlag = 1;

                // 先把队列里还没处理的连接全部清掉。
                // 否则线程被唤醒后，可能还会继续拿旧任务。
                while(pool.queue.size > 0){
                    int client_fd = deQueue(&pool.queue);
                    if(client_fd != -1){
                        shutdown(client_fd, SHUT_RDWR);
                        close(client_fd);
                    }
                }

                // 再把每个工作线程当前正在处理的连接主动 shutdown。
                // 这样阻塞在 recv 的线程更容易尽快返回。
                for(int i = 0; i < pool.num; i++){
                    if(pool.busy_fds[i] != -1){
                        shutdown(pool.busy_fds[i], SHUT_RDWR);
                    }
                }

                // 唤醒所有还睡在条件变量上的线程。
                pthread_cond_broadcast(&pool.cond);
                pthread_mutex_unlock(&pool.lock);

                // 监听 socket 也可以关掉了，因为服务端已经准备退出，不再接新连接。
                close(listen_fd);
                
                // 等待每个工作线程退出。
                for(int i = 0; i < pool.num; i++){
                    pthread_join(pool.thread_id_arr[i], NULL);
                }

                // 回收线程池资源。
                destroy_thread_pool(&pool);

                // 销毁数据库连接池。
                destroy_db_pool();

                // 关闭管道两端和 epoll。
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                close(epfd);

                // 最后关闭日志。
                close_log();
                return 0;
            }

            if(fd == listen_fd){
                // 有新客户端到来时，accept 会返回一个新的连接 fd。
                int conn_fd = accept(listen_fd, NULL, NULL);
                if (conn_fd == -1) {
                    LOG_WARN("接收客户端连接失败，错误码=%d", errno);
                    continue;
                }
                LOG_INFO("接收到客户端连接，客户端fd=%d", conn_fd);

                // 把新连接放进线程池任务队列。
                pthread_mutex_lock(&pool.lock);
                enQueue(&pool.queue, conn_fd);

                // 唤醒一个工作线程来处理这个新连接。
                pthread_cond_signal(&pool.cond);
                pthread_mutex_unlock(&pool.lock);
            }
        }
    }

    // 理论上正常不会走到这里，写上只是让资源回收更完整。
    close_log();
    return 0;
}
