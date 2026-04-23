#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "client_command_handle.h"
#include "client_socket.h"
#include "protocol.h"
#include "log.h"

#define BUFFER_SIZE 4096

typedef struct {
    char arg[FILE_NAME_LEN];
    char current_path[CMD_DATA_LEN];
    char token[TOKEN_LEN];
    char server_ip[64];
    char server_port[32];
} ClientTransferArgs;

/**
 * @brief  保证把一段数据完整写入本地文件
 * @param  fd 本地文件描述符
 * @param  buf 待写入数据起始地址
 * @param  len 本次需要写入的总字节数
 * @return 成功返回 0，失败返回 -1
 */
static int write_file_full(int fd, const char *buf, int len) {
    int total = 0;

    /* 循环写入，直到本次缓冲区内的数据全部进入本地文件。 */
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
 * @brief  在独立传输连接上执行一次 gets 下载
 * @param  sock_fd 独立传输连接 fd
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
static int run_gets_transfer(int sock_fd, const char *arg) {
    command_packet_t cmd_packet;
    file_packet_t server_file_packet;
    struct stat st;
    off_t local_size = 0;
    off_t request_offset = 0;
    int local_file_exists = 0;
    int fd = -1;
    char buf[BUFFER_SIZE];

    /* 第一步：发送 gets 命令，让服务端进入下载流程。 */
    init_command_packet(&cmd_packet, CMD_TYPE_GETS, arg);
    if (send_command_packet(sock_fd, &cmd_packet) == -1) {
        printf("发送下载命令失败\n");
        LOG_ERROR("发送下载命令失败，文件=%s", arg);
        return 0;
    }

    /* 第二步：接收服务端返回的文件基础信息。 */
    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        printf("接收文件信息失败\n");
        LOG_WARN("接收下载文件信息失败，文件=%s", arg);
        return 0;
    }

    /* file_size 小于 0 表示服务端没有找到目标文件。 */
    if (server_file_packet.file_size < 0) {
        printf("服务器文件不存在\n");
        LOG_WARN("服务端文件不存在，文件=%s", arg);
        return 0;
    }

    /* 第三步：检查本地是否已存在同名文件，用于决定续传位置。 */
    if (stat(arg, &st) == 0) {
        local_size = st.st_size;
        local_file_exists = 1;
    }

    /* 根据本地文件大小和服务端文件大小，计算本次请求的起始偏移量。 */
    if (local_size < server_file_packet.file_size) {
        request_offset = local_size;
    } else if (local_file_exists && local_size == server_file_packet.file_size) {
        request_offset = server_file_packet.file_size;
    } else {
        request_offset = 0;
    }

    file_packet_t client_file_packet;

    /* 第四步：把客户端期望的续传偏移发送给服务端。 */
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

    /* 本地文件已经完整时，无需重复接收文件内容。 */
    if (local_file_exists && request_offset == server_file_packet.file_size) {
        printf("文件已存在且完整，无需下载。\n");
        LOG_INFO("下载已跳过，本地文件完整，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
        return 0;
    }

    /* 第五步：打开或创建本地文件，准备写入下载数据。 */
    fd = open(arg, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        perror("创建文件失败");
        LOG_ERROR("创建本地文件失败，文件=%s，错误码=%d", arg, errno);
        return -1;
    }

    /* 如果本次从头开始下载，需要先清空旧文件内容。 */
    if (request_offset == 0) {
        if (ftruncate(fd, 0) == -1) {
            perror("清空旧文件失败");
            LOG_ERROR("截断本地文件失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return -1;
        }
    }

    /* 第六步：把文件指针移动到本次续传应该开始写入的位置。 */
    if (lseek(fd, request_offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地文件失败，文件=%s，偏移=%lld，错误码=%d",
                  arg,
                  (long long)request_offset,
                  errno);
        close(fd);
        return -1;
    }

    /* 第七步：循环接收服务端文件内容，并顺序写入本地文件。 */
    off_t remaining = server_file_packet.file_size - request_offset;
    while (remaining > 0) {
        int once = BUFFER_SIZE;
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        /* 每次严格接收 once 字节，保持客户端和服务端的文件协议一致。 */
        if (recv_full(sock_fd, buf, once) <= 0) {
            printf("下载中断，已经保留当前进度。\n");
            LOG_WARN("下载中断，文件=%s，已保存=%lld，总大小=%lld",
                     arg,
                     (long long)(server_file_packet.file_size - remaining),
                     (long long)server_file_packet.file_size);
            close(fd);
            return 0;
        }

        /* 把刚收到的数据完整写入本地文件。 */
        if (write_file_full(fd, buf, once) == -1) {
            perror("写入本地文件失败");
            LOG_ERROR("写入本地文件失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return -1;
        }

        remaining -= once;
    }

    /* 第八步：全部接收完成后关闭文件并输出结果。 */
    close(fd);
    printf("下载成功: %s (%ld 字节)\n", arg, (long)server_file_packet.file_size);
    LOG_INFO("下载成功，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
    return 0;
}

/**
 * @brief  下载线程入口函数
 * @param  arg 实际上传入的是 ClientTransferArgs*
 * @return 线程退出时返回 NULL
 */
static void *gets_thread_func(void *arg) {
    ClientTransferArgs *transfer_args = (ClientTransferArgs *)arg;
    int sock_fd = -1;
    auth_packet_t auth_packet;

    if (transfer_args == NULL) {
        return NULL;
    }

    /* 第一步：建立独立传输连接。 */
    init_socket(&sock_fd, transfer_args->server_ip, transfer_args->server_port);

    /* 第二步：发送认证包，让服务端恢复用户身份和当前目录。 */
    init_auth_packet(&auth_packet, CMD_TYPE_GETS, transfer_args->current_path, transfer_args->token);
    if (send_auth_packet(sock_fd, &auth_packet) == -1) {
        printf("发送下载认证信息失败\n");
        close(sock_fd);
        free(transfer_args);
        return NULL;
    }

    /* 第三步：在独立传输连接上完成下载协议。 */
    run_gets_transfer(sock_fd, transfer_args->arg);

    /* 第四步：关闭连接并释放线程参数。 */
    close(sock_fd);
    free(transfer_args);
    return NULL;
}

/**
 * @brief  处理 gets 命令，启动独立下载线程
 * @param  ctx 客户端上下文
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_gets_command(ClientAppContext *ctx, const char *arg) {
    pthread_t tid;
    ClientTransferArgs *transfer_args = NULL;
    int ret = 0;

    if (ctx == NULL || arg == NULL) {
        return -1;
    }

    if (!ctx->is_logged_in || ctx->token[0] == '\0') {
        printf("请先登录后再下载文件。\n");
        return -1;
    }

    /* 第一步：分配独立线程参数，保存本次下载所需的全部上下文。 */
    transfer_args = (ClientTransferArgs *)calloc(1, sizeof(ClientTransferArgs));
    if (transfer_args == NULL) {
        return -1;
    }

    /* 第二步：复制文件名、路径、token 和服务端地址。 */
    strncpy(transfer_args->arg, arg, sizeof(transfer_args->arg) - 1);
    strncpy(transfer_args->current_path, ctx->current_path, sizeof(transfer_args->current_path) - 1);
    strncpy(transfer_args->token, ctx->token, sizeof(transfer_args->token) - 1);
    strncpy(transfer_args->server_ip, ctx->server_ip, sizeof(transfer_args->server_ip) - 1);
    strncpy(transfer_args->server_port, ctx->server_port, sizeof(transfer_args->server_port) - 1);

    /* 第三步：创建后台下载线程，主线程继续保留给短命令使用。 */
    ret = pthread_create(&tid, NULL, gets_thread_func, transfer_args);
    if (ret != 0) {
        free(transfer_args);
        printf("创建下载线程失败\n");
        return -1;
    }

    /* 第四步：将线程设置为分离状态。 */
    pthread_detach(tid);
    printf("已启动下载任务: %s\n", arg);
    LOG_INFO("客户端已启动独立下载线程，文件=%s，当前路径=%s", arg, ctx->current_path);
    return 0;
}
