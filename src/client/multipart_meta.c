#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "multipart_meta.h"
#include "log.h"

#define MULTIPART_ROOT_DIR_NAME ".multipart"

/**
 * @brief  确保指定目录存在，不存在时递归创建
 * @param  dir 目录路径
 * @return 成功返回 0，失败返回 -1
 */
static int ensure_dir_recursive(const char *dir) {
    char buf[MAX_PATH_LEN] = {0};
    size_t len = 0;
    size_t i = 0;

    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }

    if (snprintf(buf, sizeof(buf), "%s", dir) >= (int)sizeof(buf)) {
        return -1;
    }

    // 先把目标目录路径完整复制到本地缓冲区。
    // 后面会在这个缓冲区上临时截断字符串，逐层创建父目录。
    len = strlen(buf);
    if (len == 0) {
        return -1;
    }

    // 这里从左到右扫描每一个 '/'。
    // 每遇到一个 '/'，就把它临时改成 '\0'，
    // 这样 buf 就会变成一层真实目录路径，例如：
    // 1. /a
    // 2. /a/b
    // 3. /a/b/c
    // 然后逐层 mkdir，最后再把 '/' 补回去。
    for (i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0777) == -1 && errno != EEXIST) {
                return -1;
            }
            buf[i] = '/';
        }
    }

    if (mkdir(buf, 0777) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/**
 * @brief  拼接多点下载任务根目录和元数据文件路径
 * @param  client_file_dir 客户端统一文件目录
 * @param  file_hash 目标文件 SHA-256
 * @param  task_dir 输出参数，用来保存任务目录
 * @param  task_dir_size task_dir 缓冲区大小
 * @param  meta_file_path 输出参数，用来保存元数据文件路径
 * @param  meta_file_size meta_file_path 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int build_task_paths(const char *client_file_dir, const char *file_hash,
                            char *task_dir, size_t task_dir_size,
                            char *meta_file_path, size_t meta_file_size) {
    char multipart_root[MAX_PATH_LEN] = {0};

    if (client_file_dir == NULL || file_hash == NULL ||
        task_dir == NULL || task_dir_size == 0 ||
        meta_file_path == NULL || meta_file_size == 0) {
        return -1;
    }

    // 第一步：先拼出统一的多点下载根目录：
    // test/client_files/.multipart
    if (snprintf(multipart_root, sizeof(multipart_root), "%s/%s",
                 client_file_dir,
                 MULTIPART_ROOT_DIR_NAME) >= (int)sizeof(multipart_root)) {
        return -1;
    }

    // 第二步：确保 .multipart 根目录存在。
    if (ensure_dir_recursive(multipart_root) != 0) {
        return -1;
    }

    // 第三步：再按文件 hash 拼出当前任务自己的子目录。
    // 这样同一个文件下次继续下载时，就能复用同一个任务目录。
    if (snprintf(task_dir, task_dir_size, "%s/%s", multipart_root, file_hash) >= (int)task_dir_size) {
        return -1;
    }

    // 第四步：确保当前任务目录存在。
    if (ensure_dir_recursive(task_dir) != 0) {
        return -1;
    }

    // 第五步：最后再拼出这个任务目录下的元数据文件路径。
    if (snprintf(meta_file_path, meta_file_size, "%s/meta.ini", task_dir) >= (int)meta_file_size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  根据任务目录和分片编号拼接分片临时文件路径
 * @param  task_dir 当前任务目录
 * @param  part_index 分片编号
 * @param  part_file_path 输出参数，用来保存最终路径
 * @param  path_size part_file_path 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int build_part_file_path(const char *task_dir, int part_index,
                                char *part_file_path, size_t path_size) {
    if (task_dir == NULL || part_file_path == NULL || path_size == 0) {
        return -1;
    }

    // 这里先做一次长度兜底判断。
    // part_%d 这部分长度很短，但还是先粗略留出余量，避免后面 snprintf 才发现空间明显不够。
    if (strlen(task_dir) + 16 >= path_size) {
        return -1;
    }

    // 当前分片临时文件统一命名为：
    // 任务目录/part_分片编号
    if (snprintf(part_file_path, path_size, "%s/part_%d", task_dir, part_index) >= (int)path_size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  按当前数据源数量平均切分文件范围
 * @param  file_size 目标文件总大小
 * @param  plan_packet 控制服务器返回的下载方案
 * @param  meta 输出参数，用来保存切分结果
 * @return 成功返回 0，失败返回 -1
 */
static int init_parts_from_plan(off_t file_size, const multi_gets_plan_packet_t *plan_packet,
                                MultipartTaskMeta *meta) {
    off_t base_size = 0;
    off_t remainder = 0;
    off_t current_start = 0;
    int i = 0;

    if (file_size < 0 || plan_packet == NULL || meta == NULL ||
        plan_packet->source_count <= 0 || plan_packet->source_count > MAX_SOURCE_SERVER_COUNT) {
        return -1;
    }

    meta->part_count = plan_packet->source_count;
    base_size = file_size / meta->part_count;
    remainder = file_size % meta->part_count;

    // 第五期第一版使用最直观的平均切分策略：
    // 1. 每个数据源先分到 base_size 字节
    // 2. 如果有余数，就从前往后给前几个分片各补 1 字节
    // 这样总字节数刚好能完整覆盖整个文件。
    for (i = 0; i < meta->part_count; ++i) {
        off_t current_size = base_size;

        if (remainder > 0) {
            current_size++;
            remainder--;
        }

        // 下面这几步是在填写“第 i 个分片应该负责什么”：
        // 1. 它自己的编号
        // 2. 它负责的起止区间
        // 3. 它对应的数据源地址
        // 4. 它自己的本地临时文件路径
        meta->parts[i].part_index = i;
        meta->parts[i].range_start = current_start;
        meta->parts[i].range_end = current_start + current_size - 1;
        meta->parts[i].finished_bytes = 0;
        snprintf(meta->parts[i].source_ip, sizeof(meta->parts[i].source_ip), "%s", plan_packet->sources[i].ip);
        snprintf(meta->parts[i].source_port, sizeof(meta->parts[i].source_port), "%s", plan_packet->sources[i].port);

        if (build_part_file_path(meta->task_dir,
                                 i,
                                 meta->parts[i].part_file_path,
                                 sizeof(meta->parts[i].part_file_path)) != 0) {
            return -1;
        }

        current_start = meta->parts[i].range_end + 1;
    }

    return 0;
}

/**
 * @brief  把当前任务元数据完整写回 meta.ini
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
static int save_meta_file(const MultipartTaskMeta *meta) {
    FILE *fp = NULL;
    int i = 0;

    if (meta == NULL) {
        return -1;
    }

    fp = fopen(meta->meta_file_path, "w");
    if (fp == NULL) {
        return -1;
    }

    // 先写任务级别的公共信息。
    // 这些字段可以帮助客户端下次再次执行 gets 时判断：
    // 当前任务目录到底是不是同一个文件的继续下载。
    fprintf(fp, "file_name=%s\n", meta->file_name);
    fprintf(fp, "file_hash=%s\n", meta->file_hash);
    fprintf(fp, "file_size=%lld\n", (long long)meta->file_size);
    fprintf(fp, "part_count=%d\n", meta->part_count);

    // 再逐个写入每个分片的状态。
    // 这样客户端下次恢复任务时，就知道每个分片已经完成到了哪里。
    for (i = 0; i < meta->part_count; ++i) {
        fprintf(fp, "part_%d_start=%lld\n", i, (long long)meta->parts[i].range_start);
        fprintf(fp, "part_%d_end=%lld\n", i, (long long)meta->parts[i].range_end);
        fprintf(fp, "part_%d_finished=%lld\n", i, (long long)meta->parts[i].finished_bytes);
        fprintf(fp, "part_%d_ip=%s\n", i, meta->parts[i].source_ip);
        fprintf(fp, "part_%d_port=%s\n", i, meta->parts[i].source_port);
    }

    fclose(fp);
    return 0;
}

/**
 * @brief  从元数据文件中读取一个字符串值
 * @param  line 当前整行文本
 * @param  prefix 目标前缀，例如 "file_name="
 * @param  output 输出参数，用来保存结果
 * @param  output_size output 缓冲区大小
 * @return 匹配并成功读取返回 0，否则返回 -1
 */
static int parse_meta_string_line(const char *line, const char *prefix,
                                  char *output, size_t output_size) {
    size_t prefix_len = 0;

    if (line == NULL || prefix == NULL || output == NULL || output_size == 0) {
        return -1;
    }

    // 这里的解析方式很直接：
    // 如果当前行以前缀开头，就把前缀后面的部分当成真正的值复制出来。
    prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0) {
        return -1;
    }

    if (snprintf(output, output_size, "%s", line + prefix_len) >= (int)output_size) {
        return -1;
    }

    // fgets 读进来的行尾一般还带着 '\n'。
    // 这里统一截掉，避免后面字符串比较时带着换行符。
    output[strcspn(output, "\r\n")] = '\0';
    return 0;
}

/**
 * @brief  从元数据文件中读取一个整型或偏移量值
 * @param  line 当前整行文本
 * @param  prefix 目标前缀
 * @param  output 输出参数，用来保存结果
 * @return 匹配并成功读取返回 0，否则返回 -1
 */
static int parse_meta_offset_line(const char *line, const char *prefix, off_t *output) {
    size_t prefix_len = 0;

    if (line == NULL || prefix == NULL || output == NULL) {
        return -1;
    }

    // 偏移量字段和字符串字段一样，也先判断前缀是否匹配。
    prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0) {
        return -1;
    }

    // 前缀后面剩下的就是数字文本，直接转成 off_t 保存。
    *output = (off_t)atoll(line + prefix_len);
    return 0;
}

/**
 * @brief  从已有 meta.ini 中恢复任务信息
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
static int load_meta_file(MultipartTaskMeta *meta) {
    FILE *fp = NULL;
    char line[256] = {0};
    int i = 0;

    if (meta == NULL) {
        return -1;
    }

    fp = fopen(meta->meta_file_path, "r");
    if (fp == NULL) {
        return -1;
    }

    // 这里按行扫描 meta.ini。
    // 每一行只负责一个字段，命中哪个前缀就更新哪个字段。
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (parse_meta_string_line(line, "file_name=", meta->file_name, sizeof(meta->file_name)) == 0) {
            continue;
        }

        if (parse_meta_string_line(line, "file_hash=", meta->file_hash, sizeof(meta->file_hash)) == 0) {
            continue;
        }

        if (parse_meta_offset_line(line, "file_size=", &meta->file_size) == 0) {
            continue;
        }

        if (strncmp(line, "part_count=", 11) == 0) {
            meta->part_count = atoi(line + 11);
            continue;
        }

        // 分片字段是成组出现的，例如：
        // part_0_start=
        // part_0_end=
        // part_0_finished=
        // 这里逐个尝试当前行属于哪一个分片的哪一种字段。
        for (i = 0; i < MAX_SOURCE_SERVER_COUNT; ++i) {
            char prefix[64] = {0};

            snprintf(prefix, sizeof(prefix), "part_%d_start=", i);
            if (parse_meta_offset_line(line, prefix, &meta->parts[i].range_start) == 0) {
                meta->parts[i].part_index = i;
                break;
            }

            snprintf(prefix, sizeof(prefix), "part_%d_end=", i);
            if (parse_meta_offset_line(line, prefix, &meta->parts[i].range_end) == 0) {
                meta->parts[i].part_index = i;
                break;
            }

            snprintf(prefix, sizeof(prefix), "part_%d_finished=", i);
            if (parse_meta_offset_line(line, prefix, &meta->parts[i].finished_bytes) == 0) {
                meta->parts[i].part_index = i;
                break;
            }

            snprintf(prefix, sizeof(prefix), "part_%d_ip=", i);
            if (parse_meta_string_line(line, prefix, meta->parts[i].source_ip, sizeof(meta->parts[i].source_ip)) == 0) {
                meta->parts[i].part_index = i;
                break;
            }

            snprintf(prefix, sizeof(prefix), "part_%d_port=", i);
            if (parse_meta_string_line(line, prefix, meta->parts[i].source_port, sizeof(meta->parts[i].source_port)) == 0) {
                meta->parts[i].part_index = i;
                break;
            }
        }
    }

    fclose(fp);
    return 0;
}

/**
 * @brief  根据当前任务目录中的分片文件真实大小刷新 finished_bytes
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
static int sync_finished_bytes_from_part_files(MultipartTaskMeta *meta) {
    struct stat st;
    int i = 0;

    if (meta == NULL) {
        return -1;
    }

    for (i = 0; i < meta->part_count; ++i) {
        meta->parts[i].finished_bytes = 0;

        if (stat(meta->parts[i].part_file_path, &st) == 0) {
            off_t part_total_size = meta->parts[i].range_end - meta->parts[i].range_start + 1;

            // 这里不是直接无脑使用 st_size。
            // 因为如果某些异常情况下分片文件偏大，
            // 元数据里也只应该认为它最多完成到了“理论分片总长度”。
            if (st.st_size < part_total_size) {
                meta->parts[i].finished_bytes = st.st_size;
            } else {
                meta->parts[i].finished_bytes = part_total_size;
            }
        }
    }

    return 0;
}

/**
 * @brief  判断已有 meta.ini 是否仍然匹配当前下载方案
 * @param  meta 当前任务元数据
 * @param  plan_packet 控制服务器返回的下载方案
 * @return 匹配返回 1，不匹配返回 0
 */
static int is_meta_compatible(const MultipartTaskMeta *meta, const multi_gets_plan_packet_t *plan_packet) {
    int i = 0;

    if (meta == NULL || plan_packet == NULL) {
        return 0;
    }

    if (strcmp(meta->file_name, plan_packet->file_name) != 0) {
        return 0;
    }

    if (strcmp(meta->file_hash, plan_packet->file_hash) != 0) {
        return 0;
    }

    if (meta->file_size != plan_packet->file_size) {
        return 0;
    }

    if (meta->part_count <= 0 || meta->part_count > MAX_SOURCE_SERVER_COUNT ||
        meta->part_count != plan_packet->source_count) {
        return 0;
    }

    // 第五期里，分片数和数据源地址是一一对应的。
    // 如果服务端这次返回的数据源列表已经变了，就直接按最新方案重建元数据。
    // 这样可以避免客户端继续拿旧地址去请求新的分片任务。
    for (i = 0; i < meta->part_count; ++i) {
        if (strcmp(meta->parts[i].source_ip, plan_packet->sources[i].ip) != 0 ||
            strcmp(meta->parts[i].source_port, plan_packet->sources[i].port) != 0) {
            return 0;
        }
    }

    return 1;
}

/**
 * @brief  根据服务端返回的下载方案，准备本地多点下载任务目录和元数据
 * @param  client_file_dir 客户端统一文件目录
 * @param  plan_packet 控制服务器返回的多点下载方案
 * @param  meta 输出参数，用来保存当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
int multipart_task_prepare(const char *client_file_dir,
                           const multi_gets_plan_packet_t *plan_packet,
                           MultipartTaskMeta *meta) {
    int i = 0;

    if (client_file_dir == NULL || plan_packet == NULL || meta == NULL) {
        return -1;
    }

    memset(meta, 0, sizeof(MultipartTaskMeta));

    if (build_task_paths(client_file_dir,
                         plan_packet->file_hash,
                         meta->task_dir,
                         sizeof(meta->task_dir),
                         meta->meta_file_path,
                         sizeof(meta->meta_file_path)) != 0) {
        return -1;
    }

    // 第一步：先尝试恢复已有任务。
    // 如果本地已经有同 hash 的任务元数据，就优先继续使用它。
    // 只有在“读元数据失败”或“元数据和当前服务端方案不兼容”时，才重新初始化。
    if (access(meta->meta_file_path, F_OK) == 0) {
        if (load_meta_file(meta) == 0 && is_meta_compatible(meta, plan_packet)) {
            for (i = 0; i < meta->part_count; ++i) {
                if (build_part_file_path(meta->task_dir,
                                         i,
                                         meta->parts[i].part_file_path,
                                         sizeof(meta->parts[i].part_file_path)) != 0) {
                    return -1;
                }
            }

            // 元数据文件里的 finished_bytes 只是“上次退出前保存的状态”。
            // 为了更可靠，这里再根据分片临时文件的真实大小重新刷新一遍。
            sync_finished_bytes_from_part_files(meta);
            return save_meta_file(meta);
        }
    }

    // 第二步：如果没法继续复用旧任务，就按当前服务端方案重建一份新的任务元数据。
    memset(meta, 0, sizeof(MultipartTaskMeta));
    snprintf(meta->file_name, sizeof(meta->file_name), "%s", plan_packet->file_name);
    snprintf(meta->file_hash, sizeof(meta->file_hash), "%s", plan_packet->file_hash);
    meta->file_size = plan_packet->file_size;

    if (build_task_paths(client_file_dir,
                         meta->file_hash,
                         meta->task_dir,
                         sizeof(meta->task_dir),
                         meta->meta_file_path,
                         sizeof(meta->meta_file_path)) != 0) {
        return -1;
    }

    if (init_parts_from_plan(meta->file_size, plan_packet, meta) != 0) {
        return -1;
    }

    return save_meta_file(meta);
}

/**
 * @brief  根据当前分片文件真实大小刷新元数据中的 finished_bytes，并写回文件
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
int multipart_task_sync_progress(MultipartTaskMeta *meta) {
    if (meta == NULL) {
        return -1;
    }

    // 先根据真实分片文件大小刷新内存里的 finished_bytes，
    // 再把刷新后的结果写回 meta.ini。
    if (sync_finished_bytes_from_part_files(meta) != 0) {
        return -1;
    }

    return save_meta_file(meta);
}

/**
 * @brief  清理一个多点下载任务目录及其元数据文件
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
int multipart_task_cleanup(const MultipartTaskMeta *meta) {
    int i = 0;

    if (meta == NULL) {
        return -1;
    }

    // 清理顺序很直接：
    // 1. 先删元数据文件
    // 2. 再删每个分片临时文件
    // 3. 最后尝试删除任务目录本身
    unlink(meta->meta_file_path);

    for (i = 0; i < meta->part_count; ++i) {
        unlink(meta->parts[i].part_file_path);
    }

    if (rmdir(meta->task_dir) == -1 && errno != ENOENT) {
        return -1;
    }

    return 0;
}
