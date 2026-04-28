#ifndef _STORAGE_PATHS_H_
#define _STORAGE_PATHS_H_

#include <stddef.h>

/**
 * @brief  根据运行目录定位工程中的 test 根目录
 * @param  dir 输出参数，用来保存最终 test 目录路径
 * @param  size dir 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_test_base_dir_path(char *dir, size_t size);

/**
 * @brief  根据配置项名字拼接客户端本地文件目录路径
 * @param  dir 输出参数，用来保存最终客户端文件目录路径
 * @param  size dir 缓冲区大小
 * @param  dir_name 配置中读取到的目录名字，例如 client_files
 * @return 成功返回 0，失败返回 -1
 */
int get_client_file_dir_path(char *dir, size_t size, const char *dir_name);

/**
 * @brief  根据配置项名字拼接服务端真实文件目录路径
 * @param  dir 输出参数，用来保存最终服务端真实文件目录路径
 * @param  size dir 缓冲区大小
 * @param  dir_name 配置中读取到的目录名字，例如 server_files
 * @return 成功返回 0，失败返回 -1
 */
int get_server_file_dir_path(char *dir, size_t size, const char *dir_name);

/**
 * @brief  确保指定目录存在，不存在时尝试创建
 * @param  dir 目录路径
 * @return 成功返回 0，失败返回 -1
 */
int ensure_storage_dir_exists(const char *dir);

#endif
