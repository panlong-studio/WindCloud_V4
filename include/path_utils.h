#ifndef _PATH_UTILS_H_
#define _PATH_UTILS_H_

#define SERVER_BASE_DIR "../test"
#define MAX_PATH_LEN 1024

/**
 * @brief  检查用户传来的路径参数是否基本合法
 * @param  arg 用户传入的路径参数
 * @return 合法返回 0，不合法返回 -1
 */
int check_arg_path(const char *arg);

/**
 * @brief  把当前逻辑路径转换成服务器真实路径
 * @param  res 输出参数，用来保存最终真实路径
 * @param  size res 缓冲区大小
 * @param  current_path 当前逻辑路径
 * @return 成功返回 0，失败返回 -1
 */
int get_current_real_path(char *res, int size, const char *current_path);

/**
 * @brief  把“当前逻辑路径 + 参数”拼成最终真实路径
 * @param  res 输出参数，用来保存最终真实路径
 * @param  size res 缓冲区大小
 * @param  path 当前逻辑路径
 * @param  arg 用户输入的目录名或文件名
 * @return 成功返回 0，失败返回 -1
 */
int get_real_path(char *res, int size, const char *path, const char *arg);

/**
 * @brief  更新当前逻辑路径字符串
 * @param  current_path 输入输出参数，保存并更新当前逻辑路径
 * @param  size current_path 缓冲区大小
 * @param  arg 用户输入的目录名
 * @return 成功返回 0，失败返回 -1
 */
int update_current_path(char *current_path, int size, const char *arg);

#endif
