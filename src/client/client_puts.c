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
#define CLIENT_BASE_DIR "../test/client_files"
#define CLIENT_MAX_PATH_LEN 1024

/**
 * @brief  后台上传线程使用的参数集合
 * @param  arg 本次上传的文件名
 * @param  current_path 客户端主连接当前所在的逻辑路径
 * @param  token 登录成功后主连接保存的 JWT
 * @param  server_ip 服务端 IP，供独立传输线程重新建连
 * @param  server_port 服务端端口，供独立传输线程重新建连
 * @return 无
 */
typedef struct {
    char arg[FILE_NAME_LEN];         /* 本次上传的文件名 */
    char current_path[CMD_DATA_LEN]; /* 当前逻辑路径，服务端据此恢复 current_dir_id */
    char token[TOKEN_LEN];           /* 登录成功后拿到的 token */
    char server_ip[64];              /* 服务端 IP */
    char server_port[32];            /* 服务端端口 */
} ClientTransferArgs;

/**
 * @brief  获取客户端本地测试文件目录
 * @return 成功时返回可用目录字符串，失败时退回默认 CLIENT_BASE_DIR
 */
static const char *get_client_base_dir(void) {
    /* 第一步：优先尝试“从 bin 目录启动”时能直接访问到的默认路径。 */
    if (access(CLIENT_BASE_DIR, F_OK) == 0) {
        return CLIENT_BASE_DIR;
    }

    /* 第二步：如果程序从项目根目录启动，则使用 ./test/client_files。 */
    if (access("./test/client_files", F_OK) == 0) {
        return "./test/client_files";
    }

    /* 第三步：两种启动方式都没有探测成功时，退回默认相对路径。 */
    return CLIENT_BASE_DIR;
}

/**
 * @brief  确保客户端本地测试文件目录存在
 * @param  base_dir 输出参数，用来保存最终目录路径
 * @param  size base_dir 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int ensure_client_base_dir(char *base_dir, size_t size) {
    struct stat st;
    const char *dir = get_client_base_dir();

    /* 第一步：把最终选中的客户端本地测试目录复制到输出缓冲区。 */
    if (snprintf(base_dir, size, "%s", dir) >= (int)size) {
        return -1;
    }

    /* 第二步：目录已经存在时，确认它确实是一个目录。 */
    if (stat(base_dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    /* 第三步：目录不存在时，现场创建它。 */
    if (mkdir(base_dir, 0777) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/**
 * @brief  根据文件名拼接客户端本地文件完整路径
 * @param  full_path 输出参数，用来保存最终完整路径
 * @param  size full_path 缓冲区大小
 * @param  file_name 文件名
 * @return 成功返回 0，失败返回 -1
 */
static int build_client_file_path(char *full_path, size_t size, const char *file_name) {
    char base_dir[CLIENT_MAX_PATH_LEN] = {0};

    /* 第一步：文件名为空时，无法继续拼接本地路径。 */
    if (file_name == NULL) {
        return -1;
    }

    /* 第二步：先保证客户端本地测试目录可用。 */
    if (ensure_client_base_dir(base_dir, sizeof(base_dir)) != 0) {
        return -1;
    }

    /* 第三步：把“目录 + 文件名”拼成最终的本地完整路径。 */
    if (snprintf(full_path, size, "%s/%s", base_dir, file_name) >= (int)size) {
        return -1;
    }

    return 0;
}

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
    char local_path[CLIENT_MAX_PATH_LEN] = {0};
    command_packet_t cmd_packet;
    file_packet_t client_file_packet;
    file_packet_t server_file_packet;
    char buf[BUFFER_SIZE];

    /* 第一步：拼接并打开客户端本地文件，确认上传源文件存在。 */
    if (build_client_file_path(local_path, sizeof(local_path), arg) != 0) {
        printf("拼接本地文件路径失败\n");
        return -1;
    }

    fd = open(local_path, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        LOG_WARN("打开本地上传文件失败，文件=%s，本地路径=%s，错误码=%d", arg, local_path, errno);
        return -1;
    }

    /* 第二步：读取本地文件大小，后续要发送给服务端。 */
    if (fstat(fd, &st) == -1) {
        perror("获取文件大小失败");
        LOG_ERROR("读取本地上传文件信息失败，文件=%s，本地路径=%s，错误码=%d", arg, local_path, errno);
        close(fd);
        return -1;
    }

    /* 第三步：计算文件哈希值，用于秒传和续传校验。 */
    printf("正在计算文件哈希值...\n");
    if (get_file_sha256(local_path, file_hash) == -1) {
        printf("计算文件哈希值失败\n");
        LOG_ERROR("计算文件哈希值失败，文件=%s，本地路径=%s", arg, local_path);
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
        LOG_ERROR("定位本地上传文件失败，文件=%s，本地路径=%s，偏移=%lld，错误码=%d",
                  arg,
                  local_path,
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
            /* 读文件失败或已经到达文件末尾时，结束发送循环。 */
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

    /* 线程参数为空时，当前线程无法继续工作。 */
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

    /* 客户端只有在主连接已经登录、并且保存了 token 时才允许启动长命令。 */
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
