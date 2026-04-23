#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
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
#include "db_init.h"
#include "db_pool.h"
#include "control_conn.h"
#include "time_wheel.h"

#define MAX_CONTROL_FD 65536
#define CTRL_TIMEOUT_SEC 30

// pipe_fd[0] 用来读，pipe_fd[1] 用来写。
// 父进程收到 Ctrl+C 后，会往管道里写一个字节。
// 子进程的 epoll 监听到这个字节后，就进入退出流程。
int pipe_fd[2];

typedef struct {
    int epfd;
    TimeWheel *wheel;
    ControlConn **conn_map;
} ExpireContext;

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
 * @brief  创建并初始化 timerfd
 * @return 成功返回 timerfd，失败返回 -1
 */
static int create_timer_fd(void) {
    int timer_fd = -1;
    struct itimerspec timer_spec;

    timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd == -1) {
        return -1;
    }

    memset(&timer_spec, 0, sizeof(timer_spec));
    timer_spec.it_interval.tv_sec = 1;
    timer_spec.it_interval.tv_nsec = 0;
    timer_spec.it_value.tv_sec = 1;
    timer_spec.it_value.tv_nsec = 0;

    if (timerfd_settime(timer_fd, 0, &timer_spec, NULL) == -1) {
        close(timer_fd);
        return -1;
    }

    return timer_fd;
}

/**
 * @brief  关闭并回收一条控制连接
 * @param  epfd epoll fd
 * @param  wheel 时间轮结构体地址
 * @param  conn_map 控制连接映射表
 * @param  fd 控制连接 fd
 * @return 无
 */
static void close_control_connection(int epfd, TimeWheel *wheel, ControlConn **conn_map, int fd) {
    ControlConn *conn = NULL;

    if (fd < 0 || fd >= MAX_CONTROL_FD) {
        return;
    }

    conn = conn_map[fd];
    if (conn == NULL) {
        return;
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    time_wheel_remove(wheel, conn);
    close(fd);
    free(conn);
    conn_map[fd] = NULL;
}

/**
 * @brief  时间轮超时回调函数
 * @param  conn 控制连接结构体地址
 * @param  arg 透传参数，实际类型为 ExpireContext*
 * @return 无
 */
static void handle_control_timeout(ControlConn *conn, void *arg) {
    ExpireContext *ctx = (ExpireContext *)arg;

    if (conn == NULL || ctx == NULL) {
        return;
    }

    LOG_INFO("控制连接超时，准备关闭，客户端fd=%d", conn->fd);
    close_control_connection(ctx->epfd, ctx->wheel, ctx->conn_map, conn->fd);
}

/**
 * @brief  处理 timerfd 就绪事件，推进时间轮
 * @param  timer_fd timerfd
 * @param  wheel 时间轮结构体地址
 * @param  expire_ctx 时间轮回调上下文
 * @return 无
 */
static void handle_timer_event(int timer_fd, TimeWheel *wheel, ExpireContext *expire_ctx) {
    uint64_t expired_count = 0;
    ssize_t ret = read(timer_fd, &expired_count, sizeof(expired_count));

    if (ret != (ssize_t)sizeof(expired_count)) {
        return;
    }

    while (expired_count > 0) {
        time_wheel_tick(wheel, handle_control_timeout, expire_ctx);
        expired_count--;
    }
}

/**
 * @brief  尝试把新连接注册为控制连接
 * @param  epfd epoll fd
 * @param  wheel 时间轮结构体地址
 * @param  conn_map 控制连接映射表
 * @param  conn_fd 新连接 fd
 * @return 成功返回 0，失败返回 -1
 */
static int register_control_connection(int epfd, TimeWheel *wheel, ControlConn **conn_map, int conn_fd) {
    ControlConn *conn = NULL;

    if (conn_fd >= MAX_CONTROL_FD) {
        return -1;
    }

    conn = (ControlConn *)calloc(1, sizeof(ControlConn));
    if (conn == NULL) {
        return -1;
    }

    control_conn_init(conn, conn_fd);
    conn_map[conn_fd] = conn;

    add_epoll_fd(epfd, conn_fd);
    time_wheel_add(wheel, conn);
    LOG_INFO("控制连接注册成功，客户端fd=%d", conn_fd);
    return 0;
}

/**
 * @brief  处理 listen_fd 上的新连接
 * @param  epfd epoll fd
 * @param  listen_fd 监听 fd
 * @param  wheel 时间轮结构体地址
 * @param  conn_map 控制连接映射表
 * @param  pool 传输线程池地址
 * @return 无
 */
static void handle_accept_event(int epfd, int listen_fd, TimeWheel *wheel,
                                ControlConn **conn_map, thread_pool_t *pool) {
    int conn_fd = 0;
    conn_init_packet_t init_packet;

    conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd == -1) {
        LOG_WARN("接收客户端连接失败，错误码=%d", errno);
        return;
    }
    LOG_INFO("接收到客户端连接，客户端fd=%d", conn_fd);

    // 每条连接建立后，客户端都会先发一个角色初始化包。
    // 服务端通过这里区分控制连接和传输连接。
    if (recv_conn_init_packet(conn_fd, &init_packet) <= 0) {
        LOG_WARN("接收连接初始化信息失败，客户端fd=%d", conn_fd);
        close(conn_fd);
        return;
    }

    if (init_packet.role == CONN_ROLE_CTRL) {
        if (register_control_connection(epfd, wheel, conn_map, conn_fd) != 0) {
            LOG_WARN("注册控制连接失败，客户端fd=%d", conn_fd);
            close(conn_fd);
        }
        return;
    }

    if (init_packet.role == CONN_ROLE_TRANSFER) {
        pthread_mutex_lock(&pool->lock);
        enQueue(&pool->queue, conn_fd);
        pthread_cond_signal(&pool->cond);
        pthread_mutex_unlock(&pool->lock);
        LOG_INFO("传输连接进入线程池，客户端fd=%d", conn_fd);
        return;
    }

    LOG_WARN("连接角色无效，客户端fd=%d，角色=%d", conn_fd, init_packet.role);
    close(conn_fd);
}

/**
 * @brief  处理控制连接上的命令事件
 * @param  epfd epoll fd
 * @param  wheel 时间轮结构体地址
 * @param  conn_map 控制连接映射表
 * @param  fd 控制连接 fd
 * @return 无
 */
static void handle_control_event(int epfd, TimeWheel *wheel, ControlConn **conn_map, int fd) {
    ControlConn *conn = NULL;
    command_packet_t cmd_packet;

    if (fd < 0 || fd >= MAX_CONTROL_FD) {
        return;
    }

    conn = conn_map[fd];
    if (conn == NULL) {
        return;
    }

    if (recv_command_packet(fd, &cmd_packet) <= 0) {
        LOG_INFO("控制连接断开，客户端fd=%d", fd);
        close_control_connection(epfd, wheel, conn_map, fd);
        return;
    }

    dispatch_control_command(fd, &conn->ctx, &cmd_packet);
    time_wheel_refresh(wheel, conn);
}

/**
 * @brief  服务端主函数，负责初始化配置、数据库、线程池和 epoll 主循环
 * @return 正常结束返回 0，失败返回非 0
 */
int main(){
    char ip[64] = {0};
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};
    char db_host[64] = {0};
    char db_user[64] = {0};
    char db_pwd[64] = {0};
    char db_name[64] = {0};
    int listen_fd = 0;
    int epfd = -1;
    int timer_fd = -1;
    thread_pool_t pool;
    TimeWheel wheel;
    ControlConn **conn_map = NULL;
    ExpireContext expire_ctx;

    // 忽略 SIGPIPE。
    // 这样当对端断开连接后，send 不会直接把进程打死。
    signal(SIGPIPE, SIG_IGN);

    //=====================加载配置========================
    init_log_with_fallback("INFO", "../log/server.log");

    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("server_log", log_file, sizeof(log_file), "../log/server.log");

    load_value_or_default("db_host", db_host, sizeof(db_host), "127.0.0.1");
    load_value_or_default("db_user", db_user, sizeof(db_user), "root");
    load_value_or_default("db_pwd",  db_pwd,  sizeof(db_pwd),  "123456");
    load_value_or_default("db_name", db_name, sizeof(db_name), "netdisk_db");

    //=================先初始化日志========================
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
    if (pipe(pipe_fd) != 0) {
        LOG_ERROR("创建管道失败: %s", strerror(errno));
        close_log();
        return 1;
    }

    // fork 之后会分成父子两个进程。
    // 父进程专门负责监听 Ctrl+C。
    // 子进程负责真正跑服务器。
    {
        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERROR("创建子进程失败: %s", strerror(errno));
            close(pipe_fd[0]);
            close(pipe_fd[1]);
            close_log();
            return 1;
        }

        if(pid != 0){
            signal(SIGINT, func);
            LOG_INFO("服务端父进程等待子进程退出，子进程pid=%d", (int)pid);
            wait(NULL);
            LOG_INFO("服务端父进程退出");
            exit(0);
        }
    }

    if (setpgid(0, 0) != 0) {
        LOG_WARN("设置进程组失败 errno=%d", errno);
    }

    //=================子进程继续执行服务端主逻辑========================
    init_socket(&listen_fd, ip, port);
    init_thread_pool(&pool, 5);
    time_wheel_init(&wheel, CTRL_TIMEOUT_SEC);

    conn_map = (ControlConn **)calloc(MAX_CONTROL_FD, sizeof(ControlConn *));
    if (conn_map == NULL) {
        LOG_ERROR("控制连接映射表申请失败");
        destroy_thread_pool(&pool);
        destroy_db_pool();
        close(listen_fd);
        close_log();
        return 1;
    }

    timer_fd = create_timer_fd();
    if (timer_fd == -1) {
        LOG_ERROR("创建 timerfd 失败");
        free(conn_map);
        destroy_thread_pool(&pool);
        destroy_db_pool();
        close(listen_fd);
        close_log();
        return 1;
    }

    epfd = epoll_create(1);
    ERROR_CHECK(epfd, -1, "创建 epoll");

    add_epoll_fd(epfd, listen_fd);
    add_epoll_fd(epfd, pipe_fd[0]);
    add_epoll_fd(epfd, timer_fd);

    expire_ctx.epfd = epfd;
    expire_ctx.wheel = &wheel;
    expire_ctx.conn_map = conn_map;

    LOG_INFO("服务端启动成功，地址=%s，端口=%s", ip, port);

    while(1){
        struct epoll_event events[32];
        int nready = epoll_wait(epfd, events, 32, -1);

        if (nready == -1) {
            if (errno == EINTR) {
                continue;
            }
            ERROR_CHECK(nready, -1, "等待 epoll 事件");
        }

        for(int idx = 0; idx < nready; idx++){
            int fd = events[idx].data.fd;

            if(fd == pipe_fd[0]){
                char buf[10];
                read(fd, buf, sizeof(buf));
                LOG_INFO("服务端收到退出信号");

                pthread_mutex_lock(&pool.lock);
                pool.exitFlag = 1;

                while(pool.queue.size > 0){
                    int client_fd = deQueue(&pool.queue);
                    if(client_fd != -1){
                        shutdown(client_fd, SHUT_RDWR);
                        close(client_fd);
                    }
                }

                for(int i = 0; i < pool.num; i++){
                    if(pool.busy_fds[i] != -1){
                        shutdown(pool.busy_fds[i], SHUT_RDWR);
                    }
                }

                pthread_cond_broadcast(&pool.cond);
                pthread_mutex_unlock(&pool.lock);

                close(listen_fd);

                for (int conn_fd = 0; conn_fd < MAX_CONTROL_FD; conn_fd++) {
                    if (conn_map[conn_fd] != NULL) {
                        close_control_connection(epfd, &wheel, conn_map, conn_fd);
                    }
                }

                for(int i = 0; i < pool.num; i++){
                    pthread_join(pool.thread_id_arr[i], NULL);
                }

                destroy_thread_pool(&pool);
                destroy_db_pool();

                close(timer_fd);
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                close(epfd);
                free(conn_map);

                close_log();
                return 0;
            }

            if(fd == timer_fd){
                handle_timer_event(timer_fd, &wheel, &expire_ctx);
                continue;
            }

            if(fd == listen_fd){
                handle_accept_event(epfd, listen_fd, &wheel, conn_map, &pool);
                continue;
            }

            handle_control_event(epfd, &wheel, conn_map, fd);
        }
    }

    close_log();
    return 0;
}
