#ifndef _DAO_FILE_H_
#define _DAO_FILE_H_

#include <sys/types.h>

/**
 * @brief  根据 SHA-256 查询 files 表，判断这个“真实文件”是否已经存在
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 *         注意：客户端上传时传来的 hash 是字符串形式，
 *         例如 "86d82221f388c7a0..."
 * @param  out_file_id 输出参数，保存 files 表主键 id
 *         调用成功后，*out_file_id 就是 files.id
 * @param  out_file_size 输出参数，保存文件大小
 *         调用成功后，*out_file_size 就是 files.size
 * @return 找到返回 0，找不到返回 -1
 */
int dao_file_find_by_sha256(const char *sha256sum, int *out_file_id, off_t *out_file_size);

/**
 * @brief  往 files 表中插入一条新的真实文件记录
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @param  file_size 文件大小
 *         这里保存的是“真实文件总大小”，不是本次传了多少字节
 * @param  out_file_id 输出参数，保存新插入记录的 id
 *         插入成功后，这个 id 会被 paths.file_id 使用
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_insert(const char *sha256sum, off_t file_size, int *out_file_id);

/**
 * @brief  将 files 表中某个文件的引用计数加 1
 * @param  file_id files 表主键 id
 *         当另一个用户通过秒传引用了同一份真实文件时，
 *         就应该把 count 加 1
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_add_ref_count(int file_id);

/**
 * @brief  将 files 表中某个文件的引用计数减 1
 * @param  file_id files 表主键 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_sub_ref_count(int file_id);

/**
 * @brief  查询 files 表中某个文件当前的引用计数
 * @param  file_id files 表主键 id
 * @param  out_ref_count 输出参数，保存 count
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_get_ref_count(int file_id, int *out_ref_count);

/**
 * @brief  删除 files 表中的一条真实文件记录
 * @param  file_id files 表主键 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_delete(int file_id);

/**
 * @brief  根据 files 表 id 取出真实文件的 SHA-256 和大小
 * @param  file_id files 表主键 id
 * @param  sha256sum_out 输出参数，用来接收 64 位十六进制 SHA-256 字符串
 *         下载时需要把它拼到 test/files/<sha256> 中，定位真实文件
 * @param  out_file_size 输出参数，用来接收文件大小
 *         下载时服务端要先把这个大小发给客户端，客户端才能决定断点位置
 * @return 成功返回 0，失败返回 -1
 */
int dao_file_get_info_by_id(int file_id, char *sha256sum_out, off_t *out_file_size);

#endif
