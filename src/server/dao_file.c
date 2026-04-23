#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "dao_file.h"
#include "db_pool.h"

/**
 * @brief  根据 SHA-256 查询 files 表
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @param  out_file_id 输出参数，保存 files.id
 * @param  out_file_size 输出参数，保存 files.size
 * @return 找到返回 0，找不到返回 -1
 */
int dao_file_find_by_sha256(const char *sha256sum, int *out_file_id, off_t *out_file_size) {
    char sql[256];

    // files.sha256sum 在数据库里是 binary(32)。
    // 但 C 代码里拿到的是 64 位十六进制字符串。
    // 所以这里要用 UNHEX()，把字符串重新转回 32 字节二进制再查。
    snprintf(sql, sizeof(sql),
        "SELECT id, size FROM files WHERE sha256sum=UNHEX('%s')",
        sha256sum);

    MYSQL_RES *res = db_execute_query(sql);
    if (res == NULL) {
        return -1;
    }

    // files.sha256sum 是唯一键。
    // 因此同一个 hash 最多只会查到一条记录，这里直接取第一行即可。
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == NULL) {
        mysql_free_result(res);
        return -1;
    }

    // row[0] -> files.id
    // row[1] -> files.size
    *out_file_id = atoi(row[0]);
    *out_file_size = (off_t)atoll(row[1]);

    mysql_free_result(res);
    return 0;
}

/**
 * @brief  插入新的真实文件记录
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @param  file_size 文件大小
 * @param  out_file_id 输出参数，保存新记录 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_insert(const char *sha256sum, off_t file_size, int *out_file_id) {
    char sql[256];

    // 一份新的真实文件第一次插入时：
    // 1. size 就是这份真实文件大小
    // 2. count 初始就是 1，因为当前至少已经有一个逻辑文件引用它
    snprintf(sql, sizeof(sql),
        "INSERT INTO files (sha256sum, size, count) VALUES (UNHEX('%s'), %lld, 1)",
        sha256sum, (long long)file_size);

    if (db_execute_update(sql) != 0) {
        return -1;
    }

    // db_execute_update 只负责执行 SQL，不直接返回自增 id。
    // 为了代码保持简单，这里插入成功后再按 sha256 查一遍，把 id 取回来。
    return dao_file_find_by_sha256(sha256sum, out_file_id, &file_size);
}

/**
 * @brief  引用计数加 1
 * @param  file_id files 表主键 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_add_ref_count(int file_id) {
    char sql[256];

    // 秒传的核心之一：不再重复存文件，而是把“引用次数”加 1。
    snprintf(sql, sizeof(sql),
        "UPDATE files SET count=count+1 WHERE id=%d",
        file_id);

    return db_execute_update(sql);
}

/**
 * @brief  引用计数减 1
 * @param  file_id files 表主键 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_sub_ref_count(int file_id) {
    char sql[256];

    // 删除逻辑文件节点时，真实文件的引用数也要同步减 1。
    // 这里加上 count>0 保护，避免计数被减成负数。
    snprintf(sql, sizeof(sql),
        "UPDATE files SET count=count-1 WHERE id=%d AND count>0",
        file_id);

    return db_execute_update(sql);
}

/**
 * @brief  查询 files 表中某个文件当前的引用计数
 * @param  file_id files 表主键 id
 * @param  out_ref_count 输出参数，保存 count
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_get_ref_count(int file_id, int *out_ref_count) {
    char sql[256];

    snprintf(sql, sizeof(sql),
        "SELECT count FROM files WHERE id=%d",
        file_id);

    MYSQL_RES *res = db_execute_query(sql);
    if (res == NULL) {
        return -1;
    }

    // count 是删除回收链路里的关键字段。
    // 后续 rm 会根据这个值判断：是只删逻辑节点，还是继续回收真实文件实体。
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == NULL) {
        mysql_free_result(res);
        return -1;
    }

    *out_ref_count = atoi(row[0]);
    mysql_free_result(res);
    return 0;
}

/**
 * @brief  删除 files 表中的一条真实文件记录
 * @param  file_id files 表主键 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_delete(int file_id) {
    char sql[256];

    // 只有在 count 已经归零时，业务层才应该调用这里。
    // 这条 SQL 的职责很单一：把“无人引用的真实文件元数据”从 files 表中删掉。
    snprintf(sql, sizeof(sql),
        "DELETE FROM files WHERE id=%d",
        file_id);

    return db_execute_update(sql);
}

/**
 * @brief  根据 file_id 取出 SHA-256 和文件大小
 * @param  file_id files 表主键 id
 * @param  sha256sum_out 输出参数，保存 64 位十六进制字符串
 * @param  out_file_size 输出参数，保存文件大小
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_get_info_by_id(int file_id, char *sha256sum_out, off_t *out_file_size) {
    char sql[256];

    // 这里要特别注意格式统一问题：
    // 1. 数据库里 sha256sum 是 binary(32)
    // 2. 真实磁盘文件名是“小写十六进制字符串”
    // 3. Linux 文件名区分大小写
    // 所以这里必须统一转成小写，否则下载阶段会出现：
    // 数据库查到的是大写 hash，磁盘上却是小写文件名，最终 open 失败。
    snprintf(sql, sizeof(sql),
        "SELECT LOWER(HEX(sha256sum)), size FROM files WHERE id=%d",
        file_id);

    MYSQL_RES *res = db_execute_query(sql);
    if (res == NULL) {
        return -1;
    }

    // files.id 是主键，因此这里同样只会命中一条记录。
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == NULL) {
        mysql_free_result(res);
        return -1;
    }

    // row[0] 是小写 hash 字符串
    // row[1] 是真实文件大小
    strcpy(sha256sum_out, row[0]);
    *out_file_size = (off_t)atoll(row[1]);

    mysql_free_result(res);
    return 0;
}
