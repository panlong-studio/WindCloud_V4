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
#include "conn_manager.h"
#include "time_wheel.h"

#define SERVER_TIME_WHEEL_SIZE 60
#define SERVER_TIME_OUT_SECONDS 30

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
    // 父进程向管道写入一个字节，通知子进程进入退出流程。
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

    // 第一步：优先从配置文件读取目标键值。
    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    // 第二步：配置缺失时回退到默认值。
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

    // 第一步：根据启动目录修正日志文件相对路径。
    // 服务端既可能从项目根目录启动，也可能从 bin 目录启动，
    // 两种情况下 "../log" 与 "./log" 的可见位置不同。
    if (log_file != NULL && strncmp(log_file, "../", 3) == 0) {
        if (access("../log", F_OK) == 0) {
            real_log_file = log_file;
        } else if (access("./log", F_OK) == 0) {
            real_log_file = log_file + 3;
        }
    }

    // 第二步：优先按修正后的路径初始化日志。
    if (init_log(level_str, real_log_file) == 0) {
        return;
    }

    // 第三步：如果文件日志初始化失败，则退回控制台日志。
    init_log(level_str, NULL);
}

/**
 * @brief  关闭并清理一个仍在 epoll 管理中的连接
 * @param  epfd epoll 实例 fd
 * @param  manager 连接状态管理器
 * @param  client_fd 要关闭的客户端 fd
 * @return 无
 */
static void close_managed_connection(int epfd, ConnManager *manager, int client_fd) {
    // 第一步：把目标 fd 从 epoll 监听集合中移除。
    del_epoll_fd(epfd, client_fd);

    // 第二步：关闭连接读写方向，并释放文件描述符。
    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);

    // 第三步：从连接状态管理器中删除对应节点。
    conn_manager_remove(manager, client_fd);
}

/**
 * @brief  从 socket 里预读一个命令类型
 * @param  client_fd 当前客户端套接字
 * @param  cmd_type 输出参数，用来保存预读到的命令类型
 * @return 成功返回 0，失败返回 -1
 */
static int peek_cmd_type(int client_fd, int *cmd_type) {
    int ret = 0;

    // 第一步：校验输出参数。
    if (cmd_type == NULL) {
        return -1;
    }

    // 第二步：使用 MSG_PEEK 预读命令类型字段。
    // 这样主线程可以先判断本次连接是普通命令还是认证连接，
    // 同时不破坏后续真正的接收顺序。
    ret = recv(client_fd, cmd_type, sizeof(int), MSG_PEEK | MSG_WAITALL);
    if (ret != (int)sizeof(int)) {
        return -1;
    }

    return 0;
}

/**
 * @brief  推进时间轮，并关闭真正超时的连接
 * @param  epfd epoll 实例 fd
 * @param  manager 连接状态管理器
 * @param  wheel 时间轮对象
 * @return 无
 */
static void process_time_wheel_tick(int epfd, ConnManager *manager, TimeWheel *wheel) {
    int expired_fds[256] = {0};
    int expired_count = 0;

    // 第一步：推进时间轮，收集本轮真正超时的 fd。
    expired_count = time_wheel_tick(wheel, manager, expired_fds, 256);

    // 第二步：逐个关闭已确认超时的主连接。
    for (int idx = 0; idx < expired_count; ++idx) {
        ServerConnState *state = conn_manager_get(manager, expired_fds[idx]);
        if (state == NULL) {
            continue;
        }

        LOG_INFO("连接超时，客户端fd=%d，用户id=%d，当前路径=%s",
                 state->fd,
                 state->user_id,
                 state->current_path);
        close_managed_connection(epfd, manager, state->fd);
    }
}

/**
 * @brief  服务端主函数，负责初始化配置、数据库、线程池和 epoll 主循环
 * @return 正常结束返回 0，失败返回非 0
 */
int main(){
    // 忽略 SIGPIPE。
    // 对端断开后，send 调用将通过返回值反馈错误，不会终止当前进程。
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

    //-----------创建连接状态管理器和时间轮----------------
    ConnManager conn_manager;
    TimeWheel time_wheel;
    conn_manager_init(&conn_manager);
    if (time_wheel_init(&time_wheel, SERVER_TIME_WHEEL_SIZE, SERVER_TIME_OUT_SECONDS) != 0) {
        LOG_ERROR("时间轮初始化失败");
        destroy_thread_pool(&pool);
        destroy_db_pool();
        close(listen_fd);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        close_log();
        return 1;
    }

    //-----------创建 epoll 实例----------------
    int epfd = epoll_create(1);
    ERROR_CHECK(epfd, -1, "创建 epoll");

    // 监听 listen_fd：表示有新客户端到来。
    add_epoll_fd(epfd, listen_fd);

    // 监听 pipe_fd[0]：表示父进程通知子进程退出。
    add_epoll_fd(epfd, pipe_fd[0]);
    LOG_INFO("服务端启动成功，地址=%s，端口=%s", ip, port);

    time_t last_tick_time = time(NULL);

    // 主循环，持续等待 epoll 事件。
    while(1){
        // lst 保存本轮就绪的事件列表。
        struct epoll_event lst[32];

        // epoll_wait 最长等待 1000ms，超时后用于推进时间轮。
        int nready = epoll_wait(epfd, lst, 32, 1000);
        if (nready == -1) {
            if (errno == EINTR) {
                continue;
            }
            ERROR_CHECK(nready, -1, "等待 epoll 事件");
        }

        // 每轮事件处理前，先根据当前时间推进时间轮。
        time_t now_time = time(NULL);
        while (last_tick_time < now_time) {
            process_time_wheel_tick(epfd, &conn_manager, &time_wheel);
            last_tick_time++;
        }
        
        // 依次处理本轮全部就绪事件。
        for(int idx = 0; idx < nready; idx++){
            // 取出当前事件对应的 fd。
            int fd = lst[idx].data.fd;

            if(fd == pipe_fd[0]){
                // 读取退出通知字节。
                char buf[10];
                read(fd, buf, sizeof(buf));
                LOG_INFO("服务端收到退出信号");

                // 修改线程池共享数据前先加锁。
                pthread_mutex_lock(&pool.lock);

                // 置位退出标记，阻止线程池继续接收任务。
                pool.exitFlag = 1;

                // 清理队列中尚未处理的传输任务。
                while(pool.queue.size > 0){
                    transfer_task_t task;
                    if(deQueue(&pool.queue, &task) == 0){
                        shutdown(task.client_fd, SHUT_RDWR);
                        close(task.client_fd);
                    }
                }

                // 对工作线程正在处理的连接发起关闭流程，便于阻塞调用尽快返回。
                for(int i = 0; i < pool.num; i++){
                    if(pool.busy_fds[i] != -1){
                        shutdown(pool.busy_fds[i], SHUT_RDWR);
                    }
                }

                // 唤醒条件变量上的全部工作线程。
                pthread_cond_broadcast(&pool.cond);
                pthread_mutex_unlock(&pool.lock);

                // 服务端进入退出阶段后，不再接受新连接。
                close(listen_fd);
                
                // 等待全部工作线程退出。
                for(int i = 0; i < pool.num; i++){
                    pthread_join(pool.thread_id_arr[i], NULL);
                }

                // 释放线程池、连接状态和时间轮资源。
                destroy_thread_pool(&pool);
                conn_manager_destroy(&conn_manager);
                time_wheel_destroy(&time_wheel);

                // 释放数据库连接池。
                destroy_db_pool();

                // 关闭管道和 epoll 实例。
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                close(epfd);

                // 最后关闭日志系统。
                close_log();
                return 0;
            }

            if(fd == listen_fd){
                // accept 返回新的客户端连接 fd。
                int conn_fd = accept(listen_fd, NULL, NULL);
                if (conn_fd == -1) {
                    LOG_WARN("接收客户端连接失败，错误码=%d", errno);
                    continue;
                }
                LOG_INFO("接收到客户端连接，客户端fd=%d", conn_fd);

                // 第一步：把新连接加入 epoll。
                add_epoll_fd(epfd, conn_fd);

                // 第二步：为新连接建立默认状态。
                ServerConnState *state = conn_manager_add(&conn_manager, conn_fd);
                if (state == NULL) {
                    del_epoll_fd(epfd, conn_fd);
                    close(conn_fd);
                    continue;
                }

                // 第三步：把新连接加入时间轮。
                time_wheel_refresh(&time_wheel, state);
                continue;
            }

            // 非 listen_fd 事件统一按“已登记连接”处理。
            ServerConnState *state = conn_manager_get(&conn_manager, fd);
            if (state == NULL) {
                continue;
            }

            int cmd_type = 0;

            // 预读命令类型失败通常表示连接已关闭或协议不完整。
            if (peek_cmd_type(fd, &cmd_type) != 0) {
                LOG_INFO("客户端连接断开，客户端fd=%d", fd);
                close_managed_connection(epfd, &conn_manager, fd);
                continue;
            }

            if (cmd_type == CMD_TYPE_AUTH) {
                auth_packet_t auth_packet;
                transfer_task_t task;

                // 第一步：读取认证包。
                if (recv_auth_packet(fd, &auth_packet) <= 0) {
                    close_managed_connection(epfd, &conn_manager, fd);
                    continue;
                }

                // 第二步：根据认证包和后续命令整理传输任务。
                if (session_build_transfer_task(fd, &auth_packet, &task) != 0) {
                    close_managed_connection(epfd, &conn_manager, fd);
                    continue;
                }

                // 第三步：传输连接将交给工作线程处理，因此主线程不再继续监听该 fd。
                del_epoll_fd(epfd, fd);
                conn_manager_remove(&conn_manager, fd);

                // 第四步：把任务加入线程池队列，并唤醒一个工作线程。
                pthread_mutex_lock(&pool.lock);
                if (enQueue(&pool.queue, &task) != 0) {
                    pthread_mutex_unlock(&pool.lock);
                    shutdown(fd, SHUT_RDWR);
                    close(fd);
                    continue;
                }
                pthread_cond_signal(&pool.cond);
                pthread_mutex_unlock(&pool.lock);

                LOG_INFO("认证通过，传输任务已入队，客户端fd=%d，命令类型=%d，用户id=%d",
                         fd,
                         task.cmd_type,
                         task.ctx.user_id);
                continue;
            }

            // 普通连接按 command_packet_t 协议接收命令。
            command_packet_t cmd_packet;
            if (recv_command_packet(fd, &cmd_packet) <= 0) {
                close_managed_connection(epfd, &conn_manager, fd);
                continue;
            }

            // 主线程处理短命令，并把处理结果同步回连接状态。
            if (session_handle_main_command(fd, state, &cmd_packet) != 0) {
                close_managed_connection(epfd, &conn_manager, fd);
                continue;
            }

            // 命令处理完成后，刷新当前连接的超时位置。
            time_wheel_refresh(&time_wheel, state);
        }
    }

    // 该返回路径通常不会被执行，这里保留统一的资源回收出口。
    close_log();
    return 0;
}
