#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <errno.h>
#include "file_transfer.h"
#include "dao_file.h"
#include "dao_file_source.h"
#include "dao_vfs.h"
#include "path_utils.h"
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "config.h"
#include "server_identity.h"
#include "storage_paths.h"

#define BUFFER_SIZE 4096
#define LARGE_FILE_MMAP_THRESHOLD (100 * 1024 * 1024)

/**
 * @brief  确保真实文件仓库目录 test/server_files 存在
 * @param  store_dir 输出参数，用来保存最终可用的真实文件仓库路径
 * @param  size store_dir 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int ensure_store_dir(char *store_dir, int size) {
    char dir_name[128] = {0};

    // 真实文件统一放在 test/server_files 目录下。
    // 每个文件都不用用户原来的名字，而是直接用 sha256 值命名。
    // 这样服务器就能做到：
    // 1. 同内容文件只保存一份
    // 2. 用户目录结构和真实物理文件彻底分离
    if (get_target("server_file_dir", dir_name) != 0) {
        snprintf(dir_name, sizeof(dir_name), "%s", "server_files");
    }

    if (get_server_file_dir_path(store_dir, size, dir_name) != 0) {
        return -1;
    }

    return ensure_storage_dir_exists(store_dir);
}

/**
 * @brief  根据 sha256 值拼接真实文件完整路径
 * @param  real_path 输出参数，用来保存最终真实文件路径
 * @param  size real_path 缓冲区大小
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @return 成功返回 0，失败返回 -1
 */
static int build_store_file_path(char *real_path, int size, const char *sha256sum) {
    char store_dir[MAX_PATH_LEN] = {0};

    if (ensure_store_dir(store_dir, sizeof(store_dir)) != 0) {
        return -1;
    }

    if (snprintf(real_path, size, "%s/%s", store_dir, sha256sum) >= size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  根据当前会话目录和用户输入，拼接数据库中的逻辑全路径
 * @param  full_path 输出参数，用来保存最终逻辑路径
 * @param  size full_path 缓冲区大小
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1
 */
static int build_full_virtual_path(char *full_path, int size, ClientContext *ctx, const char *arg) {
    if (ctx == NULL || arg == NULL) {
        return -1;
    }

    // 这里复用 path_utils 里的基础路径校验。
    // 例如：
    // 1. 空字符串不允许
    // 2. ".." 不允许
    // 3. 绝对路径不允许
    if (check_arg_path(arg) == -1) {
        return -1;
    }

    // 当前这版 puts/gets 按“当前目录下的一个文件名”处理。
    // 如果这里放开多级相对路径，那么 current_dir_id 的维护方式也要一起改。
    // 为了保持成员 E 代码简单直观，这里先明确限制为单层文件名。
    if (strchr(arg, '/') != NULL) {
        return -1;
    }

    if (strcmp(ctx->current_path, "/") == 0) {
        // 当前就在根目录时，逻辑路径就是 "/文件名"
        if (snprintf(full_path, size, "/%s", arg) >= size) {
            return -1;
        }
    } else {
        // 当前不在根目录时，逻辑路径就是 "当前目录/文件名"
        if (snprintf(full_path, size, "%s/%s", ctx->current_path, arg) >= size) {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief  从逻辑全路径中提取最后一级文件名
 * @param  full_path 逻辑全路径，例如 /doc/a.txt
 * @param  file_name 输出参数，用来保存最后一级文件名，例如 a.txt
 * @return 无
 */
static void extract_file_name(const char *full_path, char *file_name) {
    const char *last_slash = strrchr(full_path, '/');

    if (last_slash == NULL) {
        strcpy(file_name, full_path);
        return;
    }

    strcpy(file_name, last_slash + 1);
}

/**
 * @brief  检查虚拟文件系统中的名字是否合法
 * @param  file_name 待校验的目录名或文件名
 * @return 合法返回 1，不合法返回 0
 */
static int is_valid_vfs_name(const char *file_name) {
    size_t len = strlen(file_name);

    if (len == 0) {
        return 0;
    }

    if (len > MAX_VFS_NAME_LEN) {
        return 0;
    }

    return 1;
}

/**
 * @brief  把一段数据完整写入真实文件
 * @param  file_fd 真实文件 fd
 * @param  buf 待写入数据起始地址
 * @param  len 本次需要写入的总字节数
 * @return 成功返回 0，失败返回 -1
 */
static int write_file_full(int file_fd, const char *buf, int len) {
    int total = 0;

    // write 不保证一次把数据全写完。
    // 这里做一个完整写入循环，避免上传接收端把文件写缺。
    while (total < len) {
        int ret = write(file_fd, buf + total, len - total);
        if (ret <= 0) {
            return -1;
        }
        total += ret;
    }

    return 0;
}

/**
 * @brief  使用 sendfile 发送真实文件的指定区间
 * @param  client_fd 当前客户端套接字
 * @param  file_fd 真实文件 fd
 * @param  offset 起始偏移位置
 * @param  file_size 文件总大小
 * @param  real_path 真实文件路径，仅用于日志
 * @return 成功返回 0，失败返回 -1
 */
static int send_file_by_sendfile(int client_fd, int file_fd, off_t offset,
                                 off_t file_size, const char *real_path) {
    off_t current_offset = offset;
    off_t remaining = file_size - offset;

    while (remaining > 0) {
        ssize_t sent = sendfile(client_fd, file_fd, &current_offset, (size_t)remaining);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            LOG_WARN("下载发送中断，客户端fd=%d，路径=%s，错误码=%d", client_fd, real_path, errno);
            return -1;
        }

        if (sent == 0) {
            LOG_WARN("下载提前结束，客户端fd=%d，路径=%s，剩余字节=%lld",
                     client_fd,
                     real_path,
                     (long long)remaining);
            return -1;
        }

        remaining -= sent;
    }

    return 0;
}

/**
 * @brief  使用 mmap 发送真实大文件的指定区间
 * @param  client_fd 当前客户端套接字
 * @param  file_fd 真实文件 fd
 * @param  offset 起始偏移位置
 * @param  file_size 文件总大小
 * @param  real_path 真实文件路径，仅用于日志
 * @return 成功返回 0，失败返回 -1
 */
static int send_file_by_mmap(int client_fd, int file_fd, off_t offset,
                             off_t file_size, const char *real_path) {
    char *map_ptr = NULL;
    char *read_start = NULL;
    off_t remaining = file_size - offset;
    off_t sent_count = 0;

    if (remaining <= 0) {
        return 0;
    }

    // 文件超过 100M 后，按照第二期要求，改用 mmap 读取真实文件再发送。
    map_ptr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
    if (map_ptr == MAP_FAILED) {
        LOG_ERROR("下载 mmap 失败，客户端fd=%d，路径=%s，大小=%lld，错误码=%d",
                  client_fd,
                  real_path,
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

        if (send_full(client_fd, read_start + sent_count, once) == -1) {
            LOG_WARN("mmap 下载发送中断，客户端fd=%d，路径=%s，已发送=%lld，总大小=%lld",
                     client_fd,
                     real_path,
                     (long long)(offset + sent_count),
                     (long long)file_size);
            munmap(map_ptr, file_size);
            return -1;
        }

        sent_count += once;
    }

    munmap(map_ptr, file_size);
    return 0;
}

/**
 * @brief  在下载失败时向客户端发送统一的失败文件包
 * @param  client_fd 当前客户端套接字
 * @param  file_name 客户端请求下载的文件名
 * @return 无
 */
static void send_gets_failed_packet(int client_fd, const char *file_name) {
    file_packet_t server_file_packet;

    // 下载协议要求服务端先回一个 file_packet_t。
    // 客户端就是靠 file_size < 0 判断“服务器没有这个文件”。
    // 因此这里不能只发普通文本包，否则客户端协议状态会错位。
    init_file_packet(&server_file_packet, CMD_TYPE_GETS, file_name, -1, 0, NULL);
    send_file_packet(client_fd, &server_file_packet);
}

/**
 * @brief  检查某个 sha256 对应的真实文件是否完整可复用
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @param  expected_size 数据库 files.size 中记录的期望文件大小
 * @param  local_size 输出参数，用来返回磁盘上当前真实文件的大小
 * @return 返回 1 表示真实文件存在且大小匹配，可以安全秒传
 * @return 返回 0 表示真实文件缺失或大小不匹配，应退化为正常上传/续传
 * @return 返回 -1 表示校验过程本身发生异常，服务端无法继续判断
 */
static int check_store_file_ready(const char *sha256sum, off_t expected_size, off_t *local_size) {
    char real_path[MAX_PATH_LEN] = {0};
    struct stat st;

    // 先默认成 0，避免调用方在失败分支读到未初始化值。
    *local_size = 0;

    // 第一步：先把 test/server_files/<sha256> 的真实路径拼出来。
    // 如果连路径都无法构造，说明服务端存储环境本身就有问题。
    if (build_store_file_path(real_path, sizeof(real_path), sha256sum) != 0) {
        return -1;
    }

    // 第二步：检查真实文件是否存在。
    // ENOENT 说明 files 表里虽然有记录，但磁盘实体已经缺失，这时不能秒传，只能退化成正常上传。
    if (stat(real_path, &st) == -1) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    // 如果同名路径存在但不是普通文件，也说明真实仓库状态异常。
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }

    // 第三步：把磁盘上的真实大小回传给调用方。
    // 后面如果需要降级为正常上传，可以直接复用这个大小作为续传偏移的参考。
    *local_size = st.st_size;

    // 只有“文件存在 + 大小与 files.size 完全一致”时，才能证明当前真实文件是完整的。
    // 否则就算数据库有 hash 记录，也不能把它当成可秒传实体继续复用。
    if (st.st_size == expected_size) {
        return 1;
    }

    return 0;
}

/**
 * @brief  为当前用户创建逻辑文件节点，并在需要时增加真实文件引用计数
 * @param  ctx 当前客户端会话上下文
 * @param  full_path 逻辑全路径
 * @param  file_name 最后一级文件名
 * @param  file_id files 表中的真实文件 id
 * @param  need_add_ref 是否需要额外给 files.count 加 1
 * @return 成功返回 0，失败返回 -1
 */
static int create_user_file_link(ClientContext *ctx, const char *full_path, const char *file_name,
                                 int file_id, int need_add_ref) {
    // 先插入 paths 记录。
    // 这一步的意义是：
    // 让“这个用户在这个目录下看到了这个文件”。
    if (dao_create_file_node(ctx->user_id, full_path, ctx->current_dir_id, file_name, file_id) != 0) {
        return -1;
    }

    if (need_add_ref) {
        // 只有在“真实文件本来就存在”的情况下，才需要把引用计数加 1。
        // 例如：
        // 1. 秒传
        // 2. 并发下别的线程已经先插入了同一个 files 记录
        if (dao_file_add_ref_count(file_id) != 0) {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief  在上传数据完整落盘后，补齐 files 表与 paths 表的数据库关系
 * @param  ctx 当前客户端会话上下文
 * @param  full_path 逻辑全路径
 * @param  file_name 最后一级文件名
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @param  file_size 真实文件大小
 * @return 成功返回 0，失败返回 -1
 */
static int finish_upload_db_work(ClientContext *ctx, const char *full_path, const char *file_name,
                                 const char *sha256sum, off_t file_size) {
    int file_id = 0;
    off_t db_file_size = 0;
    char current_ip[64] = {0};
    char current_port[16] = {0};
    int ret = -1;

    // 先尝试把这份真实文件作为“新文件”插入 files 表。
    // 这是“首次上传某个新内容”的标准分支。
    // 插入成功时，files.count 初始就是 1，因此后面补 paths 节点时无需再次加引用计数。
    if (dao_file_insert(sha256sum, file_size, &file_id) == 0) {
        ret = create_user_file_link(ctx, full_path, file_name, file_id, 0);
    } else if (dao_file_find_by_sha256(sha256sum, &file_id, &db_file_size) == 0) {
        // 如果插入失败，最常见的情况是：
        // 同一时刻别的线程已经插入了同一个 hash。
        // 那当前线程就退化为：
        // 1. 再查一次 file_id
        // 2. 给当前用户补一条 paths
        // 3. 再把 count +1
        ret = create_user_file_link(ctx, full_path, file_name, file_id, 1);
    }

    if (ret != 0) {
        return -1;
    }

    // 第五期以后，服务端除了保存真实文件和逻辑路径关系，
    // 还要把“当前服务器拥有这份真实文件”登记到 file_sources 表里。
    if (get_current_server_address(current_ip, sizeof(current_ip), current_port, sizeof(current_port)) != 0) {
        return -1;
    }

    if (dao_file_source_upsert(file_id, current_ip, current_port) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief  处理下载请求，可选择发送完整文件或指定区间
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @param  use_range 是否启用指定区间发送
 * @param  range_start 指定区间的起始位置
 * @param  range_end 指定区间的结束位置
 * @return 无
 */
static void handle_gets_common(int client_fd, ClientContext *ctx, char *arg,
                               int use_range, off_t range_start, off_t range_end) {
    char full_path[512] = {0};
    char real_path[MAX_PATH_LEN] = {0};
    char sha256sum[65] = {0};
    int node_id = 0;
    int file_id = 0;
    off_t file_size = 0;
    off_t effective_start = 0;
    off_t effective_end = 0;
    off_t logical_total_size = 0;

    if (build_full_virtual_path(full_path, sizeof(full_path), ctx, arg) == -1) {
        LOG_WARN("下载路径非法，客户端fd=%d，当前路径=%s，参数=%s", client_fd, ctx->current_path, arg);
        send_gets_failed_packet(client_fd, arg);
        return;
    }

    // 第一步：去 paths 表里查这个逻辑路径。
    // 这里查出来的 file_id，是后面通往真实文件的桥。
    if (dao_get_file_info_by_path(ctx->user_id, full_path, &node_id, &file_id) != 0) {
        LOG_WARN("下载目标不存在，客户端fd=%d，用户=%d，逻辑路径=%s", client_fd, ctx->user_id, full_path);
        send_gets_failed_packet(client_fd, arg);
        return;
    }

    // 第二步：根据 file_id 去 files 表里查真正的 sha256 和大小。
    if (dao_file_get_info_by_id(file_id, sha256sum, &file_size) != 0) {
        LOG_WARN("下载查询 files 表失败，客户端fd=%d，file_id=%d", client_fd, file_id);
        send_gets_failed_packet(client_fd, arg);
        return;
    }

    // 第三步：真实磁盘文件名其实就是 sha256。
    if (build_store_file_path(real_path, sizeof(real_path), sha256sum) != 0) {
        LOG_ERROR("拼接真实文件路径失败，客户端fd=%d，sha256=%s", client_fd, sha256sum);
        send_gets_failed_packet(client_fd, arg);
        return;
    }

    // 第四步：真正去磁盘上打开这份真实文件。
    // 到这里，下载链路才算从“逻辑路径 -> files 元数据”走到了最终物理实体。
    int file_fd = open(real_path, O_RDONLY);
    if (file_fd == -1) {
        LOG_WARN("打开真实文件失败，客户端fd=%d，路径=%s，错误码=%d", client_fd, real_path, errno);
        send_gets_failed_packet(client_fd, arg);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        LOG_ERROR("读取下载文件状态失败，客户端fd=%d，路径=%s，错误码=%d", client_fd, real_path, errno);
        close(file_fd);
        send_gets_failed_packet(client_fd, arg);
        return;
    }

    effective_start = 0;
    effective_end = st.st_size - 1;

    // 第五期多点下载会给每个传输线程分配一个固定区间。
    // 如果当前连接携带的是区间票据，这里就把真正可发送的范围收紧到票据允许的片段。
    if (use_range) {
        if (range_start < 0 || range_end < range_start || range_end >= st.st_size) {
            LOG_WARN("下载区间非法，客户端fd=%d，路径=%s，区间=%lld-%lld，文件大小=%lld",
                     client_fd,
                     real_path,
                     (long long)range_start,
                     (long long)range_end,
                     (long long)st.st_size);
            close(file_fd);
            send_gets_failed_packet(client_fd, arg);
            return;
        }

        effective_start = range_start;
        effective_end = range_end;
    }

    logical_total_size = effective_end - effective_start + 1;

    // 先把文件大小发给客户端，客户端才知道自己该从哪里续传。
    file_packet_t server_file_packet;
    init_file_packet(&server_file_packet, CMD_TYPE_GETS, arg, logical_total_size, 0, NULL);

    if (send_file_packet(client_fd, &server_file_packet) == -1) {
        LOG_WARN("发送下载文件信息失败，客户端fd=%d，路径=%s", client_fd, real_path);
        close(file_fd);
        return;
    }

    // 再收客户端的断点续传位置。
    file_packet_t client_file_packet;
    if (recv_file_packet(client_fd, &client_file_packet) <= 0) {
        LOG_WARN("接收下载断点位置失败，客户端fd=%d，路径=%s", client_fd, real_path);
        close(file_fd);
        return;
    }

    // 客户端回传的 offset 表示“本地已经下载完成的字节数”。
    // 在第五期分片下载里，这个 offset 仍然是“本分片已经完成的字节数”。
    off_t offset = client_file_packet.offset;

    if (offset < 0 || offset > logical_total_size) {
        // 如果客户端给出的断点非法，最简单安全的处理方式就是从头开始发。
        offset = 0;
    }

    offset += effective_start;

    LOG_DEBUG("准备下载真实文件，客户端fd=%d，逻辑路径=%s，真实路径=%s，发送区间=%lld-%lld，起始偏移=%lld，总大小=%lld",
              client_fd, full_path, real_path,
              (long long)effective_start,
              (long long)effective_end,
              (long long)offset,
              (long long)logical_total_size);

    // 第五步：根据文件大小选择发送方式。
    // 小文件继续使用 sendfile，大文件改成 mmap 发送。
    if (logical_total_size > LARGE_FILE_MMAP_THRESHOLD) {
        LOG_INFO("下载文件超过 100M，改用 mmap 发送，客户端fd=%d，路径=%s，区间=%lld-%lld，大小=%lld",
                 client_fd,
                 real_path,
                 (long long)effective_start,
                 (long long)effective_end,
                 (long long)logical_total_size);
        if (send_file_by_mmap(client_fd, file_fd, offset, effective_end + 1, real_path) != 0) {
            close(file_fd);
            return;
        }
    } else {
        if (send_file_by_sendfile(client_fd, file_fd, offset, effective_end + 1, real_path) != 0) {
            close(file_fd);
            return;
        }
    }

    close(file_fd);
    LOG_INFO("下载完成，客户端fd=%d，逻辑路径=%s，真实路径=%s，区间=%lld-%lld",
             client_fd,
             full_path,
             real_path,
             (long long)effective_start,
             (long long)effective_end);
}

/**
 * @brief  处理 gets 命令，按逻辑路径查找并向客户端发送完整文件
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_gets(int client_fd, ClientContext *ctx, char *arg) {
    handle_gets_common(client_fd, ctx, arg, 0, 0, -1);
}

/**
 * @brief  处理指定字节区间的 gets 下载请求
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @param  range_start 本次允许发送的起始位置
 * @param  range_end 本次允许发送的结束位置
 * @return 无
 */
void handle_gets_range(int client_fd, ClientContext *ctx, char *arg, off_t range_start, off_t range_end) {
    handle_gets_common(client_fd, ctx, arg, 1, range_start, range_end);
}

/**
 * @brief  处理 puts 命令，支持普通上传、断点续传和秒传
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_puts(int client_fd, ClientContext *ctx, char *arg) {
    char full_path[512] = {0};
    char file_name[256] = {0};
    char real_path[MAX_PATH_LEN] = {0};
    int old_node_id = 0;
    int old_node_type = 0;

    // puts 命令发过来以后，客户端一定还会紧跟着再发一个 file_packet_t。
    // 所以服务端这里必须先把这个结构体收掉，协议才能对齐。
    file_packet_t client_file_packet;
    if (recv_file_packet(client_fd, &client_file_packet) <= 0) {
        LOG_WARN("接收上传文件信息失败，客户端fd=%d", client_fd);
        send_msg(client_fd, "接收文件信息失败");
        return;
    }

    if (build_full_virtual_path(full_path, sizeof(full_path), ctx, arg) == -1) {
        LOG_WARN("上传路径非法，客户端fd=%d，参数=%s", client_fd, arg);
        send_msg(client_fd, "路径非法或过长");
        return;
    }

    // 从逻辑全路径中提取最后一级文件名。
    // paths.path 保存完整路径，paths.file_name 只保存最后一级名字，这里两者都要维护。
    extract_file_name(full_path, file_name);
    if (!is_valid_vfs_name(file_name)) {
        send_msg(client_fd, "错误：文件名过长，最大长度为 30");
        return;
    }

    // 上传前先查一下：当前用户当前目录下是不是已经有同名文件了。
    // 如果有，就不能再插一条重复路径记录。
    if (dao_get_node_by_path(ctx->user_id, full_path, &old_node_id, &old_node_type) == 0) {
        // 客户端这里还在等服务端回一个 file_packet_t。
        // 如果我们只回文本包，客户端会一直阻塞在 recv_file_packet。
        // 所以这里先回一个“无需继续发数据”的 file_packet_t，
        // 再补一条普通文本提示，让协议不乱套。
        file_packet_t server_file_packet;
        init_file_packet(&server_file_packet,
                         CMD_TYPE_PUTS,
                         arg,
                         client_file_packet.file_size,
                         client_file_packet.file_size,
                         NULL);

        send_file_packet(client_fd, &server_file_packet);
        send_msg(client_fd, "错误：该文件已存在");
        return;
    }

    if (client_file_packet.hash[0] == '\0') {
        // 当前这套秒传/真实落盘设计是强依赖 hash 的。
        // 如果客户端没传 hash，那么：
        // 1. 无法秒传判断
        // 2. 无法定位真实文件名
        // 所以直接拒绝。
        send_msg(client_fd, "上传失败：客户端没有提供文件哈希值");
        return;
    }

    // ==============================
    // 第一种情况：sha256 已经存在，直接秒传
    // ==============================
    int existed_file_id = 0;
    off_t existed_file_size = 0;

    if (dao_file_find_by_sha256(client_file_packet.hash, &existed_file_id, &existed_file_size) == 0) {
        off_t store_size = 0;
        int store_ready = check_store_file_ready(client_file_packet.hash, existed_file_size, &store_size);

        // 返回 -1 说明不是“文件不存在”，而是服务端当前连真实仓库状态都无法可靠判断。
        // 这种情况下继续往下走风险很大，因此直接失败返回。
        if (store_ready == -1) {
            send_msg(client_fd, "秒传失败：服务端无法校验真实文件");
            return;
        }

        if (store_ready == 1) {
            // 只有数据库记录和真实文件实体都完整时，才能真正秒传。
            if (create_user_file_link(ctx, full_path, file_name, existed_file_id, 1) != 0) {
                send_msg(client_fd, "秒传失败：数据库写入失败");
                return;
            }

            // 客户端约定：如果服务端回的 file_packet.hash 和本地 hash 一样，
            // 就把这次上传当成“秒传成功”，直接结束。
            file_packet_t server_file_packet;
            init_file_packet(&server_file_packet,
                             CMD_TYPE_PUTS,
                             arg,
                             client_file_packet.file_size,
                             0,
                             client_file_packet.hash);

            send_file_packet(client_fd, &server_file_packet);

            LOG_INFO("秒传成功，客户端fd=%d，用户=%d，逻辑路径=%s，file_id=%d",
                     client_fd, ctx->user_id, full_path, existed_file_id);
            return;
        }

        // 能走到这里，说明 files 表里虽然有 hash 记录，但真实文件缺失或大小不一致。
        // 这时不能继续秒传，而是要退化为正常上传/续传，修复这份真实实体。
        LOG_WARN("检测到真实文件缺失或不完整，转为正常上传，客户端fd=%d，hash=%s，磁盘大小=%lld，期望大小=%lld",
                 client_fd, client_file_packet.hash, (long long)store_size, (long long)existed_file_size);
    }

    // ==============================
    // 第二种情况：sha256 不存在，正常落盘
    // ==============================
    if (build_store_file_path(real_path, sizeof(real_path), client_file_packet.hash) != 0) {
        LOG_ERROR("拼接上传真实文件路径失败，客户端fd=%d，hash=%s", client_fd, client_file_packet.hash);
        send_msg(client_fd, "服务端创建真实路径失败");
        return;
    }

    // 真实文件名直接就是 hash。
    // 这样无论哪个用户上传同内容文件，最终指向的都是同一份物理实体。
    // 首次上传、续传、以及“修复半截真实文件”的场景都会复用这里。
    int file_fd = open(real_path, O_RDWR | O_CREAT, 0666);
    if (file_fd == -1) {
        LOG_ERROR("打开真实上传文件失败，客户端fd=%d，路径=%s，错误码=%d", client_fd, real_path, errno);
        send_msg(client_fd, "服务端创建文件失败");
        return;
    }

    struct stat st;
    off_t local_size = 0;

    if (fstat(file_fd, &st) == 0) {
        local_size = st.st_size;
    }

    // 如果服务端残留的临时文件反而比客户端还大，说明这个残留文件不可信。
    // 最简单的处理方式就是从 0 重新开始。
    if (local_size > client_file_packet.file_size) {
        local_size = 0;
        ftruncate(file_fd, 0);
    }

    // 先把服务端已有进度回给客户端，这样客户端就知道该从哪里继续发。
    // 这一步是上传断点续传的关键握手。
    file_packet_t server_file_packet;
    init_file_packet(&server_file_packet,
                     CMD_TYPE_PUTS,
                     arg,
                     client_file_packet.file_size,
                     local_size,
                     NULL);

    if (send_file_packet(client_fd, &server_file_packet) == -1) {
        close(file_fd);
        return;
    }

    // file_len 是客户端声明的完整文件大小。
    // remaining 是服务端当前还缺多少字节没有收齐。
    off_t file_len = client_file_packet.file_size;
    off_t remaining = file_len - local_size;

    // 如果这个 hash 文件在磁盘上其实已经完整了，只是数据库记录还没补上，
    // 那就不需要再收数据，直接补数据库即可。
    if (remaining <= 0) {
        // remaining <= 0 说明磁盘上这份 hash 文件已经完整了。
        // 这时无需再收网络数据，只需要把数据库关系补齐即可。
        if (finish_upload_db_work(ctx, full_path, file_name, client_file_packet.hash, file_len) != 0) {
            send_msg(client_fd, "上传失败：数据库写入失败");
        } else {
            send_msg(client_fd, "上传完成！");
        }
        close(file_fd);
        return;
    }

    // 为了能直接在偏移位置写数据，先把文件拉到目标总大小。
    // 后面 mmap 整个文件时，需要确保映射范围覆盖完整文件长度。
    if (ftruncate(file_fd, file_len) == -1) {
        send_msg(client_fd, "服务端扩展文件失败");
        close(file_fd);
        return;
    }

    // 空文件不需要走下面的数据接收循环。
    // 但数据库记录还是得补齐。
    if (file_len == 0) {
        // 空文件是一个特殊情况：
        // 它没有任何正文数据要 recv，
        // 但仍然应该在 files / paths 中留下记录。
        if (finish_upload_db_work(ctx, full_path, file_name, client_file_packet.hash, file_len) != 0) {
            send_msg(client_fd, "上传失败：数据库写入失败");
        } else {
            send_msg(client_fd, "上传完成！");
        }
        close(file_fd);
        return;
    }

    off_t received_count = 0;

    if (file_len > LARGE_FILE_MMAP_THRESHOLD) {
        char *map_ptr = mmap(NULL, file_len, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd, 0);
        char *write_start = NULL;

        if (map_ptr == MAP_FAILED) {
            send_msg(client_fd, "服务端内存映射失败");
            close(file_fd);
            return;
        }

        // 大于 100M 的上传文件继续走 mmap 接收路径。
        // 这样可以保持第二期要求中的“大文件使用 mmap”特点。
        write_start = map_ptr + local_size;
        while (received_count < remaining) {
            int once = BUFFER_SIZE;

            if (remaining - received_count < BUFFER_SIZE) {
                once = (int)(remaining - received_count);
            }

            ssize_t ret = recv(client_fd, write_start + received_count, once, 0);
            if (ret <= 0) {
                break;
            }

            received_count += ret;
        }

        munmap(map_ptr, file_len);
    } else {
        char buf[BUFFER_SIZE];

        // 小文件接收继续用更直观的 write 路径。
        // 这样也避免之前“所有非空文件都 mmap”的问题。
        if (lseek(file_fd, local_size, SEEK_SET) == -1) {
            send_msg(client_fd, "服务端定位文件失败");
            close(file_fd);
            return;
        }

        while (received_count < remaining) {
            int once = BUFFER_SIZE;
            ssize_t ret = 0;

            if (remaining - received_count < BUFFER_SIZE) {
                once = (int)(remaining - received_count);
            }

            ret = recv(client_fd, buf, once, 0);
            if (ret <= 0) {
                break;
            }

            if (write_file_full(file_fd, buf, (int)ret) == -1) {
                send_msg(client_fd, "服务端写入文件失败");
                close(file_fd);
                return;
            }

            received_count += ret;
        }
    }

    // 如果没收满，说明传输中断。
    // 这里把文件截断到“原来已有的部分 + 本次真正收到的部分”。
    // 下次客户端再上传相同 hash 时，就能从这个位置继续续传。
    if (received_count < remaining) {
        // 假设本次只收到了一部分数据，就把文件截断到“真实收到的位置”。
        // 这样下次客户端再上传同一个 hash 时，
        // 服务端仍然可以从这个位置继续续传，而不是把脏数据留在文件尾部。
        ftruncate(file_fd, local_size + received_count);
        send_msg(client_fd, "传输中断，已保存当前进度。");
        close(file_fd);
        return;
    }

    // 真正完整收满后，再补数据库。
    // 这一步故意放在最后，目的是确保数据库里的“文件已存在”永远对应一份完整可下载的真实实体。
    if (finish_upload_db_work(ctx, full_path, file_name, client_file_packet.hash, file_len) != 0) {
        send_msg(client_fd, "上传失败：数据库写入失败");
        close(file_fd);
        return;
    }

    send_msg(client_fd, "上传完成！");
    close(file_fd);

    LOG_INFO("上传完成，客户端fd=%d，用户=%d，逻辑路径=%s，真实路径=%s，大小=%lld",
             client_fd, ctx->user_id, full_path, real_path, (long long)file_len);
}
