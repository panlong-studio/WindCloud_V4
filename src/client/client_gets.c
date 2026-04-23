#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "client_command_handle.h"
#include "client_socket.h"
#include "protocol.h"
#include "log.h"

#define BUFFER_SIZE 4096

typedef struct {
    char server_ip[64];
    char server_port[64];
    char ticket[TRANSFER_TICKET_LEN];
    char file_name[FILE_NAME_LEN];
} GetsTask;

/**
 * @brief  保证把一段数据完整写入本地文件
 * @param  fd 本地文件描述符
 * @param  buf 待写入数据起始地址
 * @param  len 本次需要写入的总字节数
 * @return 成功返回 0，失败返回 -1
 */
static int write_file_full(int fd, const char *buf, int len) {
    int total = 0;

    // 普通 write 可能一次只写入部分字节。
    // 下载时必须保证本轮收到的网络数据全部写进文件，否则文件内容会错位。
    while (total < len) {
        int ret = write(fd, buf + total, len - total);
        if (ret <= 0) {
            return -1;
        }
        total += ret;
    }

    return 0;
}

/**
 * @brief  在传输连接上执行真正的下载流程
 * @param  sock_fd 传输连接 fd
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
static int run_gets_transfer(int sock_fd, const char *arg) {
    file_packet_t server_file_packet;
    struct stat st;
    off_t local_size = 0;
    off_t request_offset = 0;
    int local_file_exists = 0;
    file_packet_t client_file_packet;
    int fd = -1;
    char buf[BUFFER_SIZE];
    off_t remaining = 0;

    LOG_INFO("客户端传输线程开始下载文件，文件=%s", arg);

    // 传输连接的前置认证已经完成。
    // 到这里开始，服务端会直接回一个 file_packet_t。
    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        printf("接收文件信息失败\n");
        LOG_WARN("接收下载文件信息失败，文件=%s", arg);
        return 0;
    }

    if (server_file_packet.file_size < 0) {
        printf("服务器文件不存在或无法下载\n");
        LOG_WARN("服务端文件不存在或无法下载，文件=%s", arg);
        return 0;
    }

    // 第一步：检查本地是否已经存在同名文件，并计算续传偏移。
    if (stat(arg, &st) == 0) {
        local_size = st.st_size;
        local_file_exists = 1;
    }

    if (local_size < server_file_packet.file_size) {
        request_offset = local_size;
    } else if (local_file_exists && local_size == server_file_packet.file_size) {
        request_offset = server_file_packet.file_size;
    } else {
        request_offset = 0;
    }

    LOG_DEBUG("下载断点信息，文件=%s，本地大小=%lld，服务端大小=%lld，偏移=%lld",
              arg,
              (long long)local_size,
              (long long)server_file_packet.file_size,
              (long long)request_offset);

    // 第二步：把客户端本地已经拥有的数据长度告诉服务端。
    init_file_packet(&client_file_packet,
                     CMD_TYPE_GETS,
                     arg,
                     server_file_packet.file_size,
                     request_offset,
                     NULL);

    if (send_file_packet(sock_fd, &client_file_packet) == -1) {
        printf("发送续传位置失败\n");
        LOG_WARN("发送下载断点位置失败，文件=%s", arg);
        return -1;
    }

    if (local_file_exists && request_offset == server_file_packet.file_size) {
        printf("文件已存在且完整，无需下载。\n");
        LOG_INFO("下载已跳过，本地文件完整，文件=%s，大小=%lld",
                 arg,
                 (long long)server_file_packet.file_size);
        return 0;
    }

    // 第三步：准备本地文件句柄，开始接收文件内容。
    fd = open(arg, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        perror("创建文件失败");
        LOG_ERROR("创建本地文件失败，文件=%s，错误码=%d", arg, errno);
        return -1;
    }

    if (request_offset == 0) {
        if (ftruncate(fd, 0) == -1) {
            perror("清空旧文件失败");
            LOG_ERROR("截断本地文件失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return -1;
        }
    }

    if (lseek(fd, request_offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地文件失败，文件=%s，偏移=%lld，错误码=%d",
                  arg,
                  (long long)request_offset,
                  errno);
        close(fd);
        return -1;
    }

    remaining = server_file_packet.file_size - request_offset;

    // 第四步：循环接收网络数据并写入本地文件。
    while (remaining > 0) {
        int once = BUFFER_SIZE;
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        if (recv_full(sock_fd, buf, once) <= 0) {
            printf("下载中断，已经保留当前进度。\n");
            LOG_WARN("下载中断，文件=%s，已保存=%lld，总大小=%lld",
                     arg,
                     (long long)(server_file_packet.file_size - remaining),
                     (long long)server_file_packet.file_size);
            close(fd);
            return 0;
        }

        if (write_file_full(fd, buf, once) == -1) {
            perror("写入本地文件失败");
            LOG_ERROR("写入本地文件失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return -1;
        }

        remaining -= once;
    }

    close(fd);
    printf("下载成功: %s (%ld 字节)\n", arg, (long)server_file_packet.file_size);
    LOG_INFO("下载成功，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
    return 0;
}

/**
 * @brief  下载线程入口函数
 * @param  arg 线程参数，实际类型为 GetsTask*
 * @return 线程退出时返回 NULL
 */
static void *gets_thread_func(void *arg) {
    GetsTask *task = (GetsTask *)arg;
    int sock_fd = 0;
    conn_init_packet_t init_packet;
    transfer_auth_packet_t auth_packet;

    // 传输线程自己建立一条新连接。
    init_socket(&sock_fd, task->server_ip, task->server_port);

    // 第一步：告诉服务端这是一条传输连接。
    init_conn_init_packet(&init_packet, CONN_ROLE_TRANSFER);
    if (send_conn_init_packet(sock_fd, &init_packet) == -1) {
        printf("发送传输连接初始化信息失败\n");
        close(sock_fd);
        free(task);
        return NULL;
    }

    // 第二步：把控制连接预先申请到的一次性票据发给服务端。
    init_transfer_auth_packet(&auth_packet, task->ticket);
    if (send_transfer_auth_packet(sock_fd, &auth_packet) == -1) {
        printf("发送下载认证信息失败\n");
        close(sock_fd);
        free(task);
        return NULL;
    }

    // 第三步：进入真正的下载逻辑。
    run_gets_transfer(sock_fd, task->file_name);

    close(sock_fd);
    free(task);
    return NULL;
}

/**
 * @brief  处理 gets 命令，启动一个独立传输线程执行下载
 * @param  state 客户端统一状态结构体
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_gets_command(ClientState *state, const char *arg) {
    GetsTask *task = NULL;
    pthread_t tid;

    task = (GetsTask *)calloc(1, sizeof(GetsTask));
    if (task == NULL) {
        printf("创建下载任务失败\n");
        return -1;
    }

    // 先通过控制连接向服务端申请一次性传输票据。
    // 申请成功后，下载线程才真正建立独立传输连接。
    if (request_transfer_ticket(state, CMD_TYPE_GETS, arg, task->ticket, sizeof(task->ticket)) != 0) {
        free(task);
        return -1;
    }

    // 再把主线程里的共享状态复制一份给传输线程，
    // 这样后面下载线程就不需要长期持有锁。
    pthread_mutex_lock(&state->lock);
    snprintf(task->server_ip, sizeof(task->server_ip), "%s", state->server_ip);
    snprintf(task->server_port, sizeof(task->server_port), "%s", state->server_port);
    pthread_mutex_unlock(&state->lock);
    snprintf(task->file_name, sizeof(task->file_name), "%s", arg);

    if (pthread_create(&tid, NULL, gets_thread_func, task) != 0) {
        printf("创建下载线程失败\n");
        free(task);
        return -1;
    }

    pthread_detach(tid);
    printf("下载任务已启动。\n");
    return 0;
}
