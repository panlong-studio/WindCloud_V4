#ifndef _MULTIPART_META_H_
#define _MULTIPART_META_H_

#include <sys/types.h>
#include "path_utils.h"
#include "protocol.h"

typedef struct {
    int part_index;                 // 分片编号
    off_t range_start;              // 该分片负责的起始位置
    off_t range_end;                // 该分片负责的结束位置
    off_t finished_bytes;           // 该分片当前已完成的字节数
    char source_ip[64];             // 当前分片对应的数据源服务器 IP
    char source_port[16];           // 当前分片对应的数据源服务器端口
    char part_file_path[MAX_PATH_LEN]; // 当前分片在本地的临时文件路径
} MultipartPartInfo;

typedef struct {
    char file_name[FILE_NAME_LEN];          // 目标文件名
    char file_hash[65];                     // 目标文件 SHA-256
    off_t file_size;                        // 目标文件总大小
    int part_count;                         // 分片数量
    char task_dir[MAX_PATH_LEN];            // 当前任务目录
    char meta_file_path[MAX_PATH_LEN];      // 当前任务元数据文件路径
    MultipartPartInfo parts[MAX_SOURCE_SERVER_COUNT]; // 全部分片信息
} MultipartTaskMeta;

/**
 * @brief  根据服务端返回的下载方案，准备本地多点下载任务目录和元数据
 * @param  client_file_dir 客户端统一文件目录
 * @param  plan_packet 控制服务器返回的多点下载方案
 * @param  meta 输出参数，用来保存当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
int multipart_task_prepare(const char *client_file_dir,
                           const multi_gets_plan_packet_t *plan_packet,
                           MultipartTaskMeta *meta);

/**
 * @brief  根据当前分片文件真实大小刷新元数据中的 finished_bytes，并写回文件
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
int multipart_task_sync_progress(MultipartTaskMeta *meta);

/**
 * @brief  清理一个多点下载任务目录及其元数据文件
 * @param  meta 当前任务元数据
 * @return 成功返回 0，失败返回 -1
 */
int multipart_task_cleanup(const MultipartTaskMeta *meta);

#endif
