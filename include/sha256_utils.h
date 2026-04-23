#ifndef __SHA256_UTILS_H_
#define __SHA256_UTILS_H_

/**
 * @brief  计算指定文件的 SHA-256 字符串
 * @param  file_path 要计算 SHA-256 值的文件路径
 * @param  sha256_out 输出参数，用来保存 64 位十六进制 SHA-256 字符串
 * @return 成功返回 0，失败返回 -1
 */
int get_file_sha256(const char *file_path, char *sha256_out);

#endif
