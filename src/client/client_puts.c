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

#define BUFFER_SIZE 4096
// 100M 是第二期文档规定的大文件 mmap 分界线。
// 客户端作为上传发送端时，超过该阈值才使用 mmap 发送文件内容。
#define LARGE_FILE_MMAP_THRESHOLD ((off_t)100 * 1024 * 1024)

// 按当前测试要求，客户端本地上传文件也统一从 test/server_files 目录读取。
// 这里的目录名必须和服务端真实文件仓库保持一致。
#define CLIENT_FILE_DIR_NAME "server_files"

typedef struct {
    char server_ip[64];
    char server_port[64];
    char ticket[TRANSFER_TICKET_LEN];
    char file_name[FILE_NAME_LEN];
} PutsTask;

/**
 * @brief  获取客户端本地文件测试目录的根目录
 * @return 成功时返回可用的 test 目录字符串，失败时退回 ../test
 */
static const char *get_client_base_dir(void) {
    // 客户端可能从项目根目录启动，也可能从 bin 目录启动。
    // 优先探测 ../test，可以兼容在 bin 目录里执行 ./client_app。
    if (access("../test", F_OK) == 0) {
        return "../test";
    }

    // 如果从项目根目录执行 ./bin/client_app，则 ./test 才是正确目录。
    if (access("./test", F_OK) == 0) {
        return "./test";
    }

    // 两个目录都不存在时仍返回默认路径，让后续 mkdir 尝试创建。
    return "../test";
}

/**
 * @brief  确保客户端本地文件目录 test/server_files 存在
 * @param  file_dir 输出参数，用来保存最终可用的客户端本地文件目录
 * @param  size file_dir 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int ensure_client_file_dir(char *file_dir, int size) {
    // st 用于判断目标路径是否已经是一个目录。
    struct stat st;

    // base_dir 是当前启动目录下可用的 test 根目录。
    const char *base_dir = get_client_base_dir();

    // 客户端本地文件目录固定拼成 test/server_files。
    if (snprintf(file_dir, size, "%s/%s", base_dir, CLIENT_FILE_DIR_NAME) >= size) {
        return -1;
    }

    // 如果目录已经存在，只有它确实是目录时才算成功。
    if (stat(file_dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    // 目录不存在时创建它，保证后续 open/stat 能在固定目录下工作。
    if (mkdir(file_dir, 0777) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/**
 * @brief  根据用户输入的文件名拼接客户端本地文件完整路径
 * @param  local_path 输出参数，用来保存 test/server_files/<文件名>
 * @param  size local_path 缓冲区大小
 * @param  file_name 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1
 */
static int build_client_file_path(char *local_path, int size, const char *file_name) {
    // file_dir 用来保存已经确认存在的 test/server_files 目录。
    char file_dir[512] = {0};

    // 上传只支持固定测试目录下的单层文件名，避免绕出 test/server_files。
    if (file_name == NULL || file_name[0] == '\0' || strchr(file_name, '/') != NULL) {
        return -1;
    }

    // 先确保目录存在，再拼最终文件路径。
    if (ensure_client_file_dir(file_dir, sizeof(file_dir)) != 0) {
        return -1;
    }

    // 最终客户端本地路径固定为 test/server_files/<file_name>。
    if (snprintf(local_path, size, "%s/%s", file_dir, file_name) >= size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  使用 read + send 发送小于等于 100M 的上传文件
 * @param  sock_fd 传输连接 fd
 * @param  fd 本地文件 fd
 * @param  file_name 用户输入的文件名，仅用于日志
 * @param  file_size 本地文件总大小
 * @param  offset 服务端要求续传的起始偏移
 * @return 成功返回 0，中断或失败返回 -1
 */
static int send_upload_by_read(int sock_fd, int fd, const char *file_name,
                               off_t file_size, off_t offset) {
    // buf 是普通读文件缓冲区，小文件路径不使用 mmap。
    char buf[BUFFER_SIZE];

    // remaining 表示本次从 offset 后还需要发送多少字节。
    off_t remaining = file_size - offset;

    // 从服务端指定的断点位置继续读文件。
    if (lseek(fd, offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地上传文件失败，文件=%s，偏移=%lld，错误码=%d",
                  file_name,
                  (long long)offset,
                  errno);
        return -1;
    }

    // 按块读取本地文件并发送给服务端。
    while (remaining > 0) {
        // 默认每轮读取 BUFFER_SIZE 字节。
        int once = BUFFER_SIZE;

        // 最后一轮不足一个块时，只读取剩余字节。
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        // 从本地文件读取一块数据。
        int nread = read(fd, buf, once);
        if (nread <= 0) {
            return -1;
        }

        // send_full 负责把本轮读取的数据完整发送出去。
        if (send_full(sock_fd, buf, nread) == -1) {
            return -1;
        }

        // 扣减已经成功发送的数据量。
        remaining -= nread;
    }

    return 0;
}

/**
 * @brief  使用 mmap 发送超过 100M 的上传文件
 * @param  sock_fd 传输连接 fd
 * @param  fd 本地文件 fd
 * @param  file_name 用户输入的文件名，仅用于日志
 * @param  file_size 本地文件总大小
 * @param  offset 服务端要求续传的起始偏移
 * @return 成功返回 0，中断或失败返回 -1
 */
static int send_upload_by_mmap(int sock_fd, int fd, const char *file_name,
                               off_t file_size, off_t offset) {
    // remaining 表示本次从 offset 后还需要发送多少字节。
    off_t remaining = file_size - offset;

    // 空文件或无需续传正文时，不需要 mmap。
    if (file_size == 0 || remaining <= 0) {
        return 0;
    }

    // 大文件上传发送端按第二期要求切换为 mmap。
    char *map_ptr = mmap(NULL, (size_t)file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_ptr == MAP_FAILED) {
        LOG_ERROR("上传 mmap 映射失败，文件=%s，错误码=%d", file_name, errno);
        return -1;
    }

    // send_ptr 指向本次续传起点。
    char *send_ptr = map_ptr + offset;

    // 按块发送映射区内容，避免一次发送长度超过 send_full 的 int 参数范围。
    while (remaining > 0) {
        // 默认每轮发送 BUFFER_SIZE 字节。
        int once = BUFFER_SIZE;

        // 最后一轮不足一个块时，只发送剩余字节。
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        // send_full 负责处理 socket 部分发送。
        if (send_full(sock_fd, send_ptr, once) == -1) {
            munmap(map_ptr, (size_t)file_size);
            return -1;
        }

        // 指针和剩余长度同步后移，准备下一块。
        send_ptr += once;
        remaining -= once;
    }

    // 发送完成后释放映射区。
    munmap(map_ptr, (size_t)file_size);
    return 0;
}

/**
 * @brief  在传输连接上执行真正的上传流程
 * @param  sock_fd 传输连接 fd
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
static int run_puts_transfer(int sock_fd, const char *arg) {
    int fd = -1;
    struct stat st;
    // SHA-256 字符串需要 64 个十六进制字符 + 1 个 '\0'。
    char file_hash[65] = {0};
    char local_path[512] = {0};
    file_packet_t client_file_packet;
    file_packet_t server_file_packet;
    int send_ret = 0;

    LOG_INFO("客户端传输线程开始上传文件，文件=%s", arg);

    // 第一步：把用户输入的文件名转换为固定客户端本地路径。
    // 当前要求客户端文件目录也是 test/server_files。
    if (build_client_file_path(local_path, sizeof(local_path), arg) != 0) {
        printf("客户端本地文件路径非法或目录不可用。\n");
        LOG_WARN("构造客户端上传文件路径失败，文件=%s", arg);
        return -1;
    }

    // 第二步：打开 test/server_files 下的本地文件，拿到文件大小和 fd。
    fd = open(local_path, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        LOG_WARN("打开本地上传文件失败，文件=%s，路径=%s，错误码=%d", arg, local_path, errno);
        return -1;
    }

    if (fstat(fd, &st) == -1) {
        perror("获取文件大小失败");
        LOG_ERROR("读取本地上传文件信息失败，文件=%s，路径=%s，错误码=%d", arg, local_path, errno);
        close(fd);
        return -1;
    }

    // 第三步：上传协议依赖文件 hash。
    // 服务端会用它来判断秒传和断点续传。
    printf("正在计算文件哈希值...\n");
    if (get_file_sha256(local_path, file_hash) == -1) {
        printf("计算文件哈希值失败\n");
        LOG_ERROR("计算文件哈希值失败，文件=%s，路径=%s", arg, local_path);
        close(fd);
        return -1;
    }

    LOG_DEBUG("计算文件哈希值成功，文件=%s，哈希=%s", arg, file_hash);

    // 第四步：传输连接的前置认证已经完成。
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

    // 第五步：按 100M 阈值选择上传发送方式。
    // 上传方向的发送端是客户端，因此大文件必须在这里切换为 mmap。
    if (st.st_size > LARGE_FILE_MMAP_THRESHOLD) {
        send_ret = send_upload_by_mmap(sock_fd, fd, arg, st.st_size, server_file_packet.offset);
    } else {
        // 小文件继续使用原有 read + send 路径。
        send_ret = send_upload_by_read(sock_fd, fd, arg, st.st_size, server_file_packet.offset);
    }

    // 发送失败时，服务端会按已经收到的字节截断并保留断点。
    if (send_ret != 0) {
        printf("上传中断，服务端已经保留当前进度。\n");
        LOG_WARN("上传中断，文件=%s，总大小=%lld", arg, (long long)st.st_size);
        close(fd);
        return 0;
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
    run_puts_transfer(sock_fd, task->file_name);

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
    char local_path[512] = {0};

    // puts 的本地文件固定从 test/server_files 中查找。
    if (build_client_file_path(local_path, sizeof(local_path), arg) != 0) {
        printf("客户端本地文件目录不可用。\n");
        return -1;
    }

    // 只允许上传 test/server_files 下存在的文件。
    if (access(local_path, F_OK) != 0) {
        printf("本地文件不存在，无法上传。\n");
        return -1;
    }

    task = (PutsTask *)calloc(1, sizeof(PutsTask));
    if (task == NULL) {
        printf("创建上传任务失败\n");
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
