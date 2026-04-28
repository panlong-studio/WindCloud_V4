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
#include "path_utils.h"
#include "multipart_meta.h"
#include "sha256_utils.h"

#define BUFFER_SIZE 4096

typedef struct {
    int part_index;                         // 分片编号
    char server_ip[64];                     // 数据源服务器 IP
    char server_port[16];                   // 数据源服务器端口
    char ticket[TRANSFER_TICKET_LEN];       // 当前分片对应的区间票据
    char file_name[FILE_NAME_LEN];          // 目标文件名
    char part_file_path[MAX_PATH_LEN];      // 当前分片在本地的临时文件路径
    int result;                             // 当前分片最终结果：0 成功，1 未完成，-1 失败
} GetsPartTask;

typedef struct {
    MultipartTaskMeta meta;                         // 当前任务元数据
    char local_file_path[MAX_PATH_LEN];             // 最终合并后的目标文件路径
    int download_part_count;                        // 本轮需要真正联网下载的分片数量
    GetsPartTask part_tasks[MAX_SOURCE_SERVER_COUNT]; // 本轮要启动的下载分片任务
} GetsManagerTask;

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
    // 真实的本地路径统一由 test/client_files 和文件名拼出来。
    if (strchr(file_name, '/') != NULL) {
        return -1;
    }

    if (snprintf(local_path, size, "%s/%s", base_dir, file_name) >= (int)size) {
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

    // 普通 write 可能一次只写入部分字节。
    // 下载分片时也必须把本轮收到的全部数据写完整，否则分片内容会错位。
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
 * @brief  计算一个分片理论上应该完成多少字节
 * @param  task 当前分片下载任务
 * @return 返回该分片的总字节数
 */
static off_t get_part_total_size(const MultipartPartInfo *task) {
    if (task == NULL) {
        return 0;
    }

    return task->range_end - task->range_start + 1;
}

/**
 * @brief  判断一个分片是否已经完整下载
 * @param  task 当前分片信息
 * @return 完整返回 1，否则返回 0
 */
static int is_part_finished(const MultipartPartInfo *task) {
    return task != NULL && task->finished_bytes >= get_part_total_size(task);
}

/**
 * @brief  在传输连接上执行一个下载分片的真正下载流程
 * @param  sock_fd 传输连接 fd
 * @param  task 当前分片下载任务
 * @return 成功返回 0，中断未完成返回 1，失败返回 -1
 */
static int run_gets_part_transfer(int sock_fd, GetsPartTask *task) {
    file_packet_t server_file_packet;
    file_packet_t client_file_packet;
    struct stat st;
    int fd = -1;
    char buf[BUFFER_SIZE];
    off_t local_size = 0;
    off_t request_offset = 0;
    off_t remaining = 0;

    LOG_INFO("客户端开始下载分片，文件=%s，分片=%d，本地临时文件=%s",
             task->file_name,
             task->part_index,
             task->part_file_path);

    // 传输连接认证完成后，服务端会先返回当前分片的总大小。
    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        LOG_WARN("接收分片文件信息失败，文件=%s，分片=%d", task->file_name, task->part_index);
        return -1;
    }

    if (server_file_packet.file_size < 0) {
        LOG_WARN("服务端拒绝分片下载，文件=%s，分片=%d", task->file_name, task->part_index);
        return -1;
    }

    // 本地分片临时文件如果已经存在，就说明这是一次断点续传。
    // 这里先看看当前已经下载了多少字节，后面直接把这个值告诉服务端。
    if (stat(task->part_file_path, &st) == 0) {
        local_size = st.st_size;
    }

    // 分三种情况：
    // 1. 本地分片比服务端小，说明还能继续续传
    // 2. 本地分片刚好一样大，说明这个分片已经完成
    // 3. 本地分片反而更大，说明临时文件异常，最安全的处理是从头再下
    if (local_size < server_file_packet.file_size) {
        request_offset = local_size;
    } else if (local_size == server_file_packet.file_size) {
        request_offset = server_file_packet.file_size;
    } else {
        request_offset = 0;
    }

    init_file_packet(&client_file_packet,
                     CMD_TYPE_GETS,
                     task->file_name,
                     server_file_packet.file_size,
                     request_offset,
                     NULL);

    if (send_file_packet(sock_fd, &client_file_packet) == -1) {
        LOG_WARN("发送分片断点位置失败，文件=%s，分片=%d", task->file_name, task->part_index);
        return -1;
    }

    if (request_offset == server_file_packet.file_size) {
        LOG_INFO("分片已完整，无需继续下载，文件=%s，分片=%d", task->file_name, task->part_index);
        return 0;
    }

    // 到这里说明还需要继续接收数据。
    // 分片文件统一写到 .multipart 目录下，后面所有分片完成后再统一合并。
    fd = open(task->part_file_path, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        LOG_ERROR("创建分片临时文件失败，文件=%s，分片=%d，错误码=%d",
                  task->file_name,
                  task->part_index,
                  errno);
        return -1;
    }

    // 如果这次不是续传，而是从头开始，就先把旧的异常内容截断掉。
    if (request_offset == 0) {
        if (ftruncate(fd, 0) == -1) {
            close(fd);
            return -1;
        }
    }

    // 续传时把文件写指针移到本地已完成部分的后面。
    if (lseek(fd, request_offset, SEEK_SET) == -1) {
        close(fd);
        return -1;
    }

    // remaining 始终表示“这个分片还差多少字节没有下载完”。
    remaining = server_file_packet.file_size - request_offset;
    while (remaining > 0) {
        int once = BUFFER_SIZE;

        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        // 每次只收当前还缺的那一小段，避免最后一次越界读取。
        if (recv_full(sock_fd, buf, once) <= 0) {
            close(fd);
            LOG_WARN("分片下载中断，文件=%s，分片=%d", task->file_name, task->part_index);
            return 1;
        }

        // 收到的这一段必须完整写入临时文件，否则后面的字节位置会整体错位。
        if (write_file_full(fd, buf, once) == -1) {
            close(fd);
            LOG_ERROR("写入分片临时文件失败，文件=%s，分片=%d，错误码=%d",
                      task->file_name,
                      task->part_index,
                      errno);
            return -1;
        }

        remaining -= once;
    }

    close(fd);
    LOG_INFO("分片下载完成，文件=%s，分片=%d", task->file_name, task->part_index);
    return 0;
}

/**
 * @brief  分片下载线程入口函数
 * @param  arg 线程参数，实际类型为 GetsPartTask*
 * @return 线程退出时返回 NULL
 */
static void *gets_part_thread_func(void *arg) {
    GetsPartTask *task = (GetsPartTask *)arg;
    int sock_fd = 0;
    conn_init_packet_t init_packet;
    transfer_auth_packet_t auth_packet;

    // 每个分片线程都自己创建一条独立传输连接。
    // 这样多个分片之间互不影响，也不会阻塞主线程的控制连接。
    init_socket(&sock_fd, task->server_ip, task->server_port);

    // 第一步：告诉服务端这是一条传输连接。
    init_conn_init_packet(&init_packet, CONN_ROLE_TRANSFER);
    if (send_conn_init_packet(sock_fd, &init_packet) == -1) {
        task->result = -1;
        close(sock_fd);
        return NULL;
    }

    // 第二步：把本分片对应的区间票据发给服务端。
    init_transfer_auth_packet(&auth_packet, task->ticket);
    if (send_transfer_auth_packet(sock_fd, &auth_packet) == -1) {
        task->result = -1;
        close(sock_fd);
        return NULL;
    }

    // 第三步：进入真正的分片下载逻辑。
    task->result = run_gets_part_transfer(sock_fd, task);
    close(sock_fd);
    return NULL;
}

/**
 * @brief  把所有分片文件按顺序合并成最终文件
 * @param  task 当前下载管理任务
 * @return 成功返回 0，失败返回 -1
 */
static int merge_part_files(GetsManagerTask *task) {
    int out_fd = -1;
    int i = 0;

    if (task == NULL) {
        return -1;
    }

    // 先创建最终文件，并清空历史内容。
    // 合并永远从头开始写，避免旧文件残留内容混进结果里。
    out_fd = open(task->local_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd == -1) {
        return -1;
    }

    // 所有分片都下载完成后，再按分片顺序依次写入最终文件。
    // 这样最终文件的字节布局就和原文件完全一致。
    for (i = 0; i < task->meta.part_count; ++i) {
        int in_fd = -1;
        char buf[BUFFER_SIZE];
        ssize_t nread = 0;

        // 第 i 个分片在逻辑上就对应最终文件的第 i 段。
        // 所以这里必须严格按分片编号顺序依次拼接。
        in_fd = open(task->meta.parts[i].part_file_path, O_RDONLY);
        if (in_fd == -1) {
            close(out_fd);
            return -1;
        }

        // 一个分片文件可能仍然比较大，因此继续采用“读一块、写一块”的直观方式合并。
        while ((nread = read(in_fd, buf, sizeof(buf))) > 0) {
            if (write_file_full(out_fd, buf, (int)nread) == -1) {
                close(in_fd);
                close(out_fd);
                return -1;
            }
        }

        close(in_fd);
        if (nread < 0) {
            close(out_fd);
            return -1;
        }
    }

    close(out_fd);
    return 0;
}

/**
 * @brief  检查全部分片是否都已经完整
 * @param  meta 当前任务元数据
 * @return 全部完整返回 1，否则返回 0
 */
static int all_parts_finished(const MultipartTaskMeta *meta) {
    int i = 0;

    if (meta == NULL) {
        return 0;
    }

    for (i = 0; i < meta->part_count; ++i) {
        if (!is_part_finished(&meta->parts[i])) {
            return 0;
        }
    }

    return 1;
}

/**
 * @brief  在后台管理线程中执行完整的多点下载流程
 * @param  arg 线程参数，实际类型为 GetsManagerTask*
 * @return 线程退出时返回 NULL
 */
static void *gets_manager_thread_func(void *arg) {
    GetsManagerTask *task = (GetsManagerTask *)arg;
    pthread_t tids[MAX_SOURCE_SERVER_COUNT];
    int i = 0;
    int create_count = 0;
    char final_hash[65] = {0};

    // 第一步：为本轮尚未完成的分片分别创建下载线程。
    for (i = 0; i < task->download_part_count; ++i) {
        if (pthread_create(&tids[create_count], NULL, gets_part_thread_func, &task->part_tasks[i]) != 0) {
            task->part_tasks[i].result = -1;
            continue;
        }

        create_count++;
    }

    // 第二步：等待全部分片线程结束。
    // 这里统一 join，表示管理线程会等本轮所有联网分片都结束后，再决定后续动作。
    for (i = 0; i < create_count; ++i) {
        pthread_join(tids[i], NULL);
    }

    // 第三步：根据分片临时文件的真实大小刷新本地元数据。
    multipart_task_sync_progress(&task->meta);

    // 只要还有分片没完成，就保留现场，让用户下一次继续执行 gets 继续下载。
    if (!all_parts_finished(&task->meta)) {
        // 只要还有一个分片没完成，就不合并最终文件。
        // 这样下次再次执行 gets 时，还能继续复用已有的分片进度。
        printf("多点下载未完成，可再次执行 gets 继续下载。\n");
        free(task);
        return NULL;
    }

    // 第四步：所有分片都完成后，再合并成最终文件。
    if (merge_part_files(task) != 0) {
        printf("分片合并失败。\n");
        free(task);
        return NULL;
    }

    // 第五步：合并完成后计算最终文件 hash，确认内容正确。
    if (get_file_sha256(task->local_file_path, final_hash) != 0) {
        printf("校验最终文件哈希失败。\n");
        free(task);
        return NULL;
    }

    if (strcmp(final_hash, task->meta.file_hash) != 0) {
        // 分片全部下完不代表内容一定正确。
        // 这里再做一次整文件 hash 校验，避免分片错位或异常数据被误判为成功。
        printf("最终文件哈希不匹配。\n");
        LOG_WARN("多点下载最终文件哈希不匹配，文件=%s，期望=%s，实际=%s",
                 task->meta.file_name,
                 task->meta.file_hash,
                 final_hash);
        free(task);
        return NULL;
    }

    multipart_task_cleanup(&task->meta);
    printf("多点下载完成: %s\n", task->meta.file_name);
    LOG_INFO("多点下载完成，文件=%s，本地路径=%s", task->meta.file_name, task->local_file_path);
    free(task);
    return NULL;
}

/**
 * @brief  处理 gets 命令，启动一个后台管理线程执行多点下载
 * @param  state 客户端统一状态结构体
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1
 */
int handle_gets_command(ClientState *state, const char *arg) {
    multi_gets_plan_packet_t plan_packet;
    GetsManagerTask *manager_task = NULL;
    pthread_t tid;
    int i = 0;

    memset(&plan_packet, 0, sizeof(plan_packet));

    // gets 现在会启动一个后台管理线程。
    // 这里先分配整轮下载任务的管理对象，后续分片信息、元数据、最终路径都放这里。
    manager_task = (GetsManagerTask *)calloc(1, sizeof(GetsManagerTask));
    if (manager_task == NULL) {
        printf("创建下载任务失败\n");
        return -1;
    }

    if (build_client_local_file_path(manager_task->local_file_path,
                                     sizeof(manager_task->local_file_path),
                                     state->client_file_dir,
                                     arg) != 0) {
        printf("下载命令只需要输入文件名。\n");
        free(manager_task);
        return -1;
    }

    // 第一步：通过控制连接向服务端申请多点下载方案。
    if (request_multi_gets_plan(state, arg, &plan_packet) != 0) {
        free(manager_task);
        return -1;
    }

    // 第二步：根据服务端返回的文件 hash 和数据源列表，准备本地任务目录和元数据。
    if (multipart_task_prepare(state->client_file_dir, &plan_packet, &manager_task->meta) != 0) {
        printf("准备多点下载任务失败\n");
        free(manager_task);
        return -1;
    }

    // 如果本地已经存在某些分片临时文件，这一步会把真实完成进度同步回元数据。
    if (multipart_task_sync_progress(&manager_task->meta) != 0) {
        printf("刷新多点下载进度失败\n");
        free(manager_task);
        return -1;
    }

    // 第三步：只为当前还没完成的分片申请区间票据。
    // 已经完整的分片继续复用本地临时文件，不再重复下载。
    for (i = 0; i < manager_task->meta.part_count; ++i) {
        MultipartPartInfo *meta_part = &manager_task->meta.parts[i];
        GetsPartTask *part_task = NULL;

        // 已完成分片不用再申请票据，也不用再起线程，直接复用本地结果即可。
        if (is_part_finished(meta_part)) {
            continue;
        }

        part_task = &manager_task->part_tasks[manager_task->download_part_count];
        part_task->part_index = meta_part->part_index;
        snprintf(part_task->server_ip, sizeof(part_task->server_ip), "%s", meta_part->source_ip);
        snprintf(part_task->server_port, sizeof(part_task->server_port), "%s", meta_part->source_port);
        snprintf(part_task->file_name, sizeof(part_task->file_name), "%s", manager_task->meta.file_name);
        snprintf(part_task->part_file_path, sizeof(part_task->part_file_path), "%s", meta_part->part_file_path);

        // 只有控制连接能申请区间票据。
        // 票据里绑定了本分片的区间和目标数据源，后面的传输线程只能按这个范围下载。
        if (request_gets_range_ticket(state,
                                      manager_task->meta.file_name,
                                      meta_part->range_start,
                                      meta_part->range_end,
                                      meta_part->source_ip,
                                      meta_part->source_port,
                                      part_task->ticket,
                                      sizeof(part_task->ticket)) != 0) {
            printf("申请下载分片票据失败。\n");
            free(manager_task);
            return -1;
        }

        manager_task->download_part_count++;
    }

    // 前面的准备都成功后，才真正把任务交给后台线程执行。
    // 主线程这里立即返回，这样用户还能继续输入别的短命令。
    if (pthread_create(&tid, NULL, gets_manager_thread_func, manager_task) != 0) {
        printf("创建多点下载后台线程失败\n");
        free(manager_task);
        return -1;
    }

    pthread_detach(tid);
    printf("多点下载任务已启动。\n");
    return 0;
}
