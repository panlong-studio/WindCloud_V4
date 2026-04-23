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
#include "sha256_utils.h"

#define BUFFER_SIZE 4096

typedef struct {
    char arg[FILE_NAME_LEN];
    char current_path[CMD_DATA_LEN];
    char token[TOKEN_LEN];
    char server_ip[64];
    char server_port[32];
} ClientTransferArgs;

/**
 * @brief  在独立传输连接上执行一次 puts 上传
 * @param  sock_fd 独立传输连接 fd
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
static int run_puts_transfer(int sock_fd, const char *arg) {
    int fd = -1;
    struct stat st;
    char file_hash[64] = {0};
    command_packet_t cmd_packet;
    file_packet_t client_file_packet;
    file_packet_t server_file_packet;
    char buf[BUFFER_SIZE];

    /* 第一步：打开本地文件，确认上传源文件存在。 */
    fd = open(arg, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        LOG_WARN("打开本地上传文件失败，文件=%s，错误码=%d", arg, errno);
        return -1;
    }

    /* 第二步：读取本地文件大小，后续要发送给服务端。 */
    if (fstat(fd, &st) == -1) {
        perror("获取文件大小失败");
        LOG_ERROR("读取本地上传文件信息失败，文件=%s，错误码=%d", arg, errno);
        close(fd);
        return -1;
    }

    /* 第三步：计算文件哈希值，用于秒传和续传校验。 */
    printf("正在计算文件哈希值...\n");
    if (get_file_sha256(arg, file_hash) == -1) {
        printf("计算文件哈希值失败\n");
        LOG_ERROR("计算文件哈希值失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    /* 第四步：先发送 puts 命令包，告诉服务端后续进入上传流程。 */
    init_command_packet(&cmd_packet, CMD_TYPE_PUTS, arg);
    if (send_command_packet(sock_fd, &cmd_packet) == -1) {
        printf("发送上传命令失败\n");
        close(fd);
        return -1;
    }

    /* 第五步：发送文件大小、偏移和哈希值等基础信息。 */
    init_file_packet(&client_file_packet, CMD_TYPE_PUTS, arg, st.st_size, 0, file_hash);
    if (send_file_packet(sock_fd, &client_file_packet) == -1) {
        printf("发送文件信息失败\n");
        close(fd);
        return -1;
    }

    /* 第六步：接收服务端返回的续传信息或秒传结果。 */
    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        printf("接收服务端断点信息失败\n");
        close(fd);
        return -1;
    }

    /* 服务端返回相同哈希值时，说明该文件已经可直接复用。 */
    if (strcmp(server_file_packet.hash, file_hash) == 0) {
        printf("极速秒传成功。\n");
        LOG_INFO("上传已跳过，秒传完成，文件=%s，大小=%lld", arg, (long long)st.st_size);
        close(fd);
        return 0;
    }

    /* 服务端返回的偏移量异常时，回退到从头开始上传。 */
    if (server_file_packet.offset > st.st_size) {
        server_file_packet.offset = 0;
    }

    /* 第七步：把本地文件指针移动到服务端要求的续传位置。 */
    if (lseek(fd, server_file_packet.offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地上传文件失败，文件=%s，偏移=%lld，错误码=%d",
                  arg,
                  (long long)server_file_packet.offset,
                  errno);
        close(fd);
        return -1;
    }

    /* 第八步：循环读取本地文件，并通过独立连接发送给服务端。 */
    off_t remaining = st.st_size - server_file_packet.offset;
    while (remaining > 0) {
        int once = BUFFER_SIZE;
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        /* 单次最多读取 BUFFER_SIZE 字节，保证缓冲区使用简单直接。 */
        int nread = read(fd, buf, once);
        if (nread <= 0) {
            break;
        }

        /* 如果发送失败，本地线程直接结束，服务端已收到的部分由服务端自行保留。 */
        if (send_full(sock_fd, buf, nread) == -1) {
            printf("上传中断，已经发送的内容由服务端自己保留。\n");
            LOG_WARN("上传中断，文件=%s，已发送=%lld，总大小=%lld",
                     arg,
                     (long long)(st.st_size - remaining),
                     (long long)st.st_size);
            close(fd);
            return 0;
        }

        remaining -= nread;
    }

    /* 第九步：上传完成后，再接收一次服务端的最终提示信息。 */
    close(fd);
    recv_server_reply(sock_fd, NULL);
    LOG_INFO("上传成功，文件=%s", arg);
    return 0;
}

/**
 * @brief  上传线程入口函数
 * @param  arg 实际上传入的是 ClientTransferArgs*
 * @return 线程退出时返回 NULL
 */
static void *puts_thread_func(void *arg) {
    ClientTransferArgs *transfer_args = (ClientTransferArgs *)arg;
    int sock_fd = -1;
    auth_packet_t auth_packet;

    if (transfer_args == NULL) {
        return NULL;
    }

    /* 第一步：建立独立传输连接，避免占用主连接。 */
    init_socket(&sock_fd, transfer_args->server_ip, transfer_args->server_port);

    /* 第二步：发送认证包，让服务端恢复当前用户身份和所在目录。 */
    init_auth_packet(&auth_packet, CMD_TYPE_PUTS, transfer_args->current_path, transfer_args->token);
    if (send_auth_packet(sock_fd, &auth_packet) == -1) {
        printf("发送上传认证信息失败\n");
        close(sock_fd);
        free(transfer_args);
        return NULL;
    }

    /* 第三步：在独立传输连接上执行真实上传流程。 */
    run_puts_transfer(sock_fd, transfer_args->arg);

    /* 第四步：关闭连接并释放线程参数。 */
    close(sock_fd);
    free(transfer_args);
    return NULL;
}

/**
 * @brief  处理 puts 命令，启动独立上传线程
 * @param  ctx 客户端上下文
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_puts_command(ClientAppContext *ctx, const char *arg) {
    pthread_t tid;
    ClientTransferArgs *transfer_args = NULL;
    int ret = 0;

    if (ctx == NULL || arg == NULL) {
        return -1;
    }

    if (!ctx->is_logged_in || ctx->token[0] == '\0') {
        printf("请先登录后再上传文件。\n");
        return -1;
    }

    /* 第一步：为上传线程准备一份独立参数，避免和主线程共享可变缓冲区。 */
    transfer_args = (ClientTransferArgs *)calloc(1, sizeof(ClientTransferArgs));
    if (transfer_args == NULL) {
        return -1;
    }

    /* 第二步：把当前文件名、路径、token 和服务端地址完整复制到线程参数中。 */
    strncpy(transfer_args->arg, arg, sizeof(transfer_args->arg) - 1);
    strncpy(transfer_args->current_path, ctx->current_path, sizeof(transfer_args->current_path) - 1);
    strncpy(transfer_args->token, ctx->token, sizeof(transfer_args->token) - 1);
    strncpy(transfer_args->server_ip, ctx->server_ip, sizeof(transfer_args->server_ip) - 1);
    strncpy(transfer_args->server_port, ctx->server_port, sizeof(transfer_args->server_port) - 1);

    /* 第三步：创建后台上传线程，让主线程可以继续接收其它命令。 */
    ret = pthread_create(&tid, NULL, puts_thread_func, transfer_args);
    if (ret != 0) {
        free(transfer_args);
        printf("创建上传线程失败\n");
        return -1;
    }

    /* 第四步：把线程设置为分离状态，线程结束后自动回收系统资源。 */
    pthread_detach(tid);
    printf("已启动上传任务: %s\n", arg);
    LOG_INFO("客户端已启动独立上传线程，文件=%s，当前路径=%s", arg, ctx->current_path);
    return 0;
}
