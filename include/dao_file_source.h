#ifndef _DAO_FILE_SOURCE_H_
#define _DAO_FILE_SOURCE_H_

#include "protocol.h"

/**
 * @brief  为某个真实文件登记一个可用数据源服务器
 * @param  file_id files 表中的真实文件 id
 * @param  ip 数据源服务器 IP
 * @param  port 数据源服务器端口
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_source_upsert(int file_id, const char *ip, const char *port);

/**
 * @brief  查询某个真实文件当前有哪些可用数据源服务器
 * @param  file_id files 表中的真实文件 id
 * @param  sources 输出参数，用来保存查询结果
 * @param  max_count 最多允许返回多少个数据源
 * @param  out_count 输出参数，返回实际查询到的数量
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_source_list(int file_id, source_server_t *sources, int max_count, int *out_count);

/**
 * @brief  删除某个真实文件对应的全部数据源记录
 * @param  file_id files 表中的真实文件 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_source_delete_by_file_id(int file_id);

#endif
