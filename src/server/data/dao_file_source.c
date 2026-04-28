#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "dao_file_source.h"
#include "db_pool.h"

/**
 * @brief  为某个真实文件登记一个可用数据源服务器
 * @param  file_id files 表中的真实文件 id
 * @param  ip 数据源服务器 IP
 * @param  port 数据源服务器端口
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_source_upsert(int file_id, const char *ip, const char *port) {
    char sql[512];

    if (file_id <= 0 || ip == NULL || ip[0] == '\0' || port == NULL || port[0] == '\0') {
        return -1;
    }

    // 同一个 file_id 在同一台服务器上只保留一条记录。
    // 所以这里使用 ON DUPLICATE KEY UPDATE，避免重复插入。
    snprintf(sql, sizeof(sql),
             "INSERT INTO file_sources (file_id, server_ip, server_port, status) "
             "VALUES (%d, '%s', '%s', 1) "
             "ON DUPLICATE KEY UPDATE status=1",
             file_id,
             ip,
             port);

    return db_execute_update(sql);
}

/**
 * @brief  查询某个真实文件当前有哪些可用数据源服务器
 * @param  file_id files 表中的真实文件 id
 * @param  sources 输出参数，用来保存查询结果
 * @param  max_count 最多允许返回多少个数据源
 * @param  out_count 输出参数，返回实际查询到的数量
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_source_list(int file_id, source_server_t *sources, int max_count, int *out_count) {
    char sql[256];
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    int count = 0;

    if (file_id <= 0 || sources == NULL || max_count <= 0 || out_count == NULL) {
        return -1;
    }

    snprintf(sql, sizeof(sql),
             "SELECT server_ip, server_port "
             "FROM file_sources WHERE file_id=%d AND status=1 ORDER BY id ASC",
             file_id);

    res = db_execute_query(sql);
    if (res == NULL) {
        return -1;
    }

    // 查询结果按 id 从小到大读出。
    // 当前项目不做复杂的负载均衡，先保持最简单、最直观的顺序返回。
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (count >= max_count) {
            break;
        }

        // 每一行就是“这份真实文件当前能从哪台服务器获取”的一条来源信息。
        snprintf(sources[count].ip, sizeof(sources[count].ip), "%s", row[0]);
        snprintf(sources[count].port, sizeof(sources[count].port), "%s", row[1]);
        count++;
    }

    mysql_free_result(res);
    *out_count = count;
    return 0;
}

/**
 * @brief  删除某个真实文件对应的全部数据源记录
 * @param  file_id files 表中的真实文件 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_source_delete_by_file_id(int file_id) {
    char sql[256];

    if (file_id <= 0) {
        return -1;
    }

    // 当某个真实文件记录被彻底删除时，对应的数据源映射也要一起删除，
    // 否则后面还可能把一个已经不存在的文件误报成可下载来源。
    snprintf(sql, sizeof(sql), "DELETE FROM file_sources WHERE file_id=%d", file_id);
    return db_execute_update(sql);
}
