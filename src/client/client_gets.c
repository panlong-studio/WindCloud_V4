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
#define CLIENT_BASE_DIR "../test/client_files"
#define CLIENT_MAX_PATH_LEN 1024

/**
 * @brief  后台下载线程使用的参数集合
 * @param  arg 本次下载的文件名
 * @param  current_path 客户端主连接当前所在的逻辑路径
 * @param  token 登录成功后主连接保存的 JWT
 * @param  server_ip 服务端 IP，供独立传输线程重新建连
 * @param  server_port 服务端端口，供独立传输线程重新建连
 * @return 无
 */
typedef struct {
    char arg[FILE_NAME_LEN];         /* 本次下载的文件名 */
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
    char local_path[CLIENT_MAX_PATH_LEN] = {0};
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

    /* 第三步：拼接客户端本地路径，并检查是否已存在同名文件。 */
    if (build_client_file_path(local_path, sizeof(local_path), arg) != 0) {
        printf("拼接本地文件路径失败\n");
        return -1;
    }

    if (stat(local_path, &st) == 0) {
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
    fd = open(local_path, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        perror("创建文件失败");
        LOG_ERROR("创建本地文件失败，文件=%s，本地路径=%s，错误码=%d", arg, local_path, errno);
        return -1;
    }

    /* 如果本次从头开始下载，需要先清空旧文件内容。 */
    if (request_offset == 0) {
        if (ftruncate(fd, 0) == -1) {
            perror("清空旧文件失败");
            LOG_ERROR("截断本地文件失败，文件=%s，本地路径=%s，错误码=%d", arg, local_path, errno);
            close(fd);
            return -1;
        }
    }

    /* 第六步：把文件指针移动到本次续传应该开始写入的位置。 */
    if (lseek(fd, request_offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地文件失败，文件=%s，本地路径=%s，偏移=%lld，错误码=%d",
                  arg,
                  local_path,
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
            LOG_ERROR("写入本地文件失败，文件=%s，本地路径=%s，错误码=%d", arg, local_path, errno);
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

    /* 线程参数为空时，当前线程无法继续工作。 */
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

    /* 客户端只有在主连接已经登录、并且保存了 token 时才允许启动长命令。 */
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
