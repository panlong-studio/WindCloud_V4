#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include "client_command_handle.h"
#include "client_socket.h"
#include "protocol.h"
#include "log.h"
#include "sha256_utils.h"
#include "path_utils.h"

#define BUFFER_SIZE 4096
#define LARGE_FILE_MMAP_THRESHOLD (100 * 1024 * 1024)

typedef struct {
    char server_ip[64];
    char server_port[64];
    char ticket[TRANSFER_TICKET_LEN];
    char file_name[FILE_NAME_LEN];
    char local_file_path[MAX_PATH_LEN];
} PutsTask;

/**
 * @brief  根据客户端统一文件目录和文件名拼接本地完整路径
 * @param  local_path 输出参数，用来保存最终本地文件路径
 * @param  size local_path 缓冲区大小
 * @param  base_dir 客户端统一文件目录
 * @param  file_name 用户输入的文件名
 * @return 成功返回 0，失败返回 -1
 */
static int build_client_local_file_path(char *local_path, size_t size,
                                        const char *base_dir, const char *file_name) {
    if (local_path == NULL || size == 0 || base_dir == NULL || file_name == NULL) {
        return -1;
    }

    // 当前项目约定客户端只输入文件名。
    // 真正打开的本地文件统一放在 test/client_files 下。
    if (strchr(file_name, '/') != NULL) {
        return -1;
    }

    if (snprintf(local_path, size, "%s/%s", base_dir, file_name) >= (int)size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  使用 read + send 方式发送本地文件剩余内容
 * @param  sock_fd 传输连接 fd
 * @param  file_fd 本地文件 fd
 * @param  file_name 本地文件名
 * @param  file_size 本地文件总大小
 * @param  offset 服务端要求继续发送的偏移位置
 * @return 成功返回 0，传输中断返回 1，失败返回 -1
 */
static int send_file_by_read_loop(int sock_fd, int file_fd, const char *file_name,
                                  off_t file_size, off_t offset) {
    char buf[BUFFER_SIZE];
    off_t remaining = file_size - offset;

    // read + send 这条路径沿用项目原来的小文件发送方式。
    // 只有当文件没有超过 100M 时，才继续走这个更直观的实现。
    if (lseek(file_fd, offset, SEEK_SET) == -1) {
        LOG_ERROR("定位本地上传文件失败，文件=%s，偏移=%lld，错误码=%d",
                  file_name,
                  (long long)offset,
                  errno);
        return -1;
    }

    while (remaining > 0) {
        int once = BUFFER_SIZE;
        int nread = 0;

        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        nread = read(file_fd, buf, once);
        if (nread < 0) {
            LOG_ERROR("读取本地上传文件失败，文件=%s，错误码=%d", file_name, errno);
            return -1;
        }

        if (nread == 0) {
            LOG_ERROR("本地上传文件提前结束，文件=%s，剩余字节=%lld",
                      file_name,
                      (long long)remaining);
            return -1;
        }

        if (send_full(sock_fd, buf, nread) == -1) {
            LOG_WARN("上传中断，文件=%s，已发送=%lld，总大小=%lld",
                     file_name,
                     (long long)(file_size - remaining),
                     (long long)file_size);
            return 1;
        }

        remaining -= nread;
    }

    return 0;
}

/**
 * @brief  使用 mmap + send 方式发送本地大文件剩余内容
 * @param  sock_fd 传输连接 fd
 * @param  file_fd 本地文件 fd
 * @param  file_name 本地文件名
 * @param  file_size 本地文件总大小
 * @param  offset 服务端要求继续发送的偏移位置
 * @return 成功返回 0，传输中断返回 1，失败返回 -1
 */
static int send_file_by_mmap(int sock_fd, int file_fd, const char *file_name,
                             off_t file_size, off_t offset) {
    char *map_ptr = NULL;
    char *read_start = NULL;
    off_t remaining = file_size - offset;
    off_t sent_count = 0;

    if (remaining <= 0) {
        return 0;
    }

    // 文件超过 100M 后，改成先把文件映射到内存，再按块 send。
    // 这样可以补齐第二期文档中“大文件使用 mmap 传输”的要求。
    map_ptr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
    if (map_ptr == MAP_FAILED) {
        LOG_ERROR("映射本地上传文件失败，文件=%s，大小=%lld，错误码=%d",
                  file_name,
                  (long long)file_size,
                  errno);
        return -1;
    }

    read_start = map_ptr + offset;
    while (sent_count < remaining) {
        int once = BUFFER_SIZE;

        if (remaining - sent_count < BUFFER_SIZE) {
            once = (int)(remaining - sent_count);
        }

        if (send_full(sock_fd, read_start + sent_count, once) == -1) {
            LOG_WARN("mmap 上传中断，文件=%s，已发送=%lld，总大小=%lld",
                     file_name,
                     (long long)(offset + sent_count),
                     (long long)file_size);
            munmap(map_ptr, file_size);
            return 1;
        }

        sent_count += once;
    }

    munmap(map_ptr, file_size);
    return 0;
}

/**
 * @brief  在传输连接上执行真正的上传流程
 * @param  sock_fd 传输连接 fd
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
static int run_puts_transfer(int sock_fd, const char *arg, const char *local_file_path) {
    int fd = -1;
    struct stat st;
    char file_hash[64] = {0};
    file_packet_t client_file_packet;
    file_packet_t server_file_packet;
    off_t remaining = 0;
    int transfer_result = 0;

    LOG_INFO("客户端传输线程开始上传文件，文件=%s", arg);

    // 第一步：先打开本地文件，拿到文件大小和后续读文件所需的 fd。
    fd = open(local_file_path, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        LOG_WARN("打开本地上传文件失败，文件=%s，本地路径=%s，错误码=%d", arg, local_file_path, errno);
        return -1;
    }

    if (fstat(fd, &st) == -1) {
        perror("获取文件大小失败");
        LOG_ERROR("读取本地上传文件信息失败，文件=%s，本地路径=%s，错误码=%d", arg, local_file_path, errno);
        close(fd);
        return -1;
    }

    // 第二步：上传协议依赖文件 hash。
    // 服务端会用它来判断秒传和断点续传。
    printf("正在计算文件哈希值...\n");
    if (get_file_sha256(local_file_path, file_hash) == -1) {
        printf("计算文件哈希值失败\n");
        LOG_ERROR("计算文件哈希值失败，文件=%s，本地路径=%s", arg, local_file_path);
        close(fd);
        return -1;
    }

    LOG_DEBUG("计算文件哈希值成功，文件=%s，哈希=%s", arg, file_hash);

    // 第三步：传输连接的前置认证已经完成。
    // 到这里直接发送文件信息包，等待服务端返回续传位置。
    init_file_packet(&client_file_packet, CMD_TYPE_PUTS, arg, st.st_size, 0, file_hash);
    if (send_file_packet(sock_fd, &client_file_packet) == -1) {
        printf("发送文件信息失败\n");
        LOG_WARN("发送上传文件信息失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        printf("接收服务端断点信息失败\n");
        LOG_WARN("接收上传断点位置失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    if (strcmp(server_file_packet.hash, file_hash) == 0) {
        printf("极速秒传成功。\n");
        LOG_INFO("上传已直接完成，文件=%s，大小=%lld", arg, (long long)st.st_size);
        close(fd);
        return 0;
    }

    if (server_file_packet.offset > st.st_size) {
        server_file_packet.offset = 0;
    }

    LOG_DEBUG("上传断点信息，文件=%s，本地大小=%lld，偏移=%lld",
              arg,
              (long long)st.st_size,
              (long long)server_file_packet.offset);

    remaining = st.st_size - server_file_packet.offset;

    // 第五步：根据文件大小决定发送方式。
    // 小文件继续使用 read + send，大文件切换到 mmap + send。
    if (remaining > 0) {
        if (st.st_size > LARGE_FILE_MMAP_THRESHOLD) {
            LOG_INFO("上传文件超过 100M，改用 mmap 发送，文件=%s，大小=%lld",
                     arg,
                     (long long)st.st_size);
            transfer_result = send_file_by_mmap(sock_fd,
                                                fd,
                                                arg,
                                                st.st_size,
                                                server_file_packet.offset);
        } else {
            transfer_result = send_file_by_read_loop(sock_fd,
                                                     fd,
                                                     arg,
                                                     st.st_size,
                                                     server_file_packet.offset);
        }

        if (transfer_result == 1) {
            printf("上传中断，服务端已经保留当前进度。\n");
            close(fd);
            return 0;
        }

        if (transfer_result == -1) {
            close(fd);
            return -1;
        }
    }

    // 第六步：内容发送完成后，再收服务端最终文本结果。
    close(fd);
    {
        command_packet_t reply_packet;
        int ret = recv_server_reply(sock_fd, &reply_packet);
        if (ret != -1) {
            printf("%s\n", reply_packet.data);
        }
    }

    LOG_INFO("上传流程结束，文件=%s", arg);
    return 0;
}

/**
 * @brief  上传线程入口函数
 * @param  arg 线程参数，实际类型为 PutsTask*
 * @return 线程退出时返回 NULL
 */
static void *puts_thread_func(void *arg) {
    PutsTask *task = (PutsTask *)arg;
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
        printf("发送上传认证信息失败\n");
        close(sock_fd);
        free(task);
        return NULL;
    }

    // 第三步：进入真正的上传逻辑。
    run_puts_transfer(sock_fd, task->file_name, task->local_file_path);

    close(sock_fd);
    free(task);
    return NULL;
}

/**
 * @brief  处理 puts 命令，启动一个独立传输线程执行上传
 * @param  state 客户端统一状态结构体
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_puts_command(ClientState *state, const char *arg) {
    PutsTask *task = NULL;
    pthread_t tid;

    task = (PutsTask *)calloc(1, sizeof(PutsTask));
    if (task == NULL) {
        printf("创建上传任务失败\n");
        return -1;
    }

    if (build_client_local_file_path(task->local_file_path,
                                     sizeof(task->local_file_path),
                                     state->client_file_dir,
                                     arg) != 0) {
        printf("上传命令只需要输入文件名。\n");
        free(task);
        return -1;
    }

    if (access(task->local_file_path, F_OK) != 0) {
        printf("本地文件不存在，无法上传。\n");
        free(task);
        return -1;
    }

    // 先通过控制连接向服务端申请一次性传输票据。
    // 申请成功后，后面的传输线程才真正建立独立传输连接。
    if (request_transfer_ticket(state, CMD_TYPE_PUTS, arg, task->ticket, sizeof(task->ticket)) != 0) {
        free(task);
        return -1;
    }

    // 再把主线程里的共享状态复制一份给传输线程，
    // 这样后面上传线程就不需要长期持有锁。
    pthread_mutex_lock(&state->lock);
    snprintf(task->server_ip, sizeof(task->server_ip), "%s", state->server_ip);
    snprintf(task->server_port, sizeof(task->server_port), "%s", state->server_port);
    pthread_mutex_unlock(&state->lock);
    snprintf(task->file_name, sizeof(task->file_name), "%s", arg);

    if (pthread_create(&tid, NULL, puts_thread_func, task) != 0) {
        printf("创建上传线程失败\n");
        free(task);
        return -1;
    }

    pthread_detach(tid);
    printf("上传任务已启动。\n");
    return 0;
}
