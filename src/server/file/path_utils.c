#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "path_utils.h"

/**
 * @brief  获取服务端真实文件根目录
 * @return 返回当前运行环境下可用的 test 根目录
 */
static const char *get_server_base_dir(void) {
    // 项目既可能从根目录启动，也可能从 bin 目录启动。
    // 这里根据实际存在的路径动态选择，避免真实路径拼接错误。
    if (access(SERVER_BASE_DIR, F_OK) == 0) {
        return SERVER_BASE_DIR;
    }
    if (access("./test", F_OK) == 0) {
        return "./test";
    }
    return SERVER_BASE_DIR;
}

/**
 * @brief  检查客户端传入的相对路径参数是否合法
 * @param  arg 客户端传入的路径参数
 * @return 合法返回 0，不合法返回 -1
 */
int check_arg_path(const char *arg) {
    if (arg == NULL || arg[0] == '\0') {
        return -1;
    }
    // 安全防护：禁止跳出根目录
    if (strstr(arg, "..") != NULL) {
        return -1;
    }
    // 安全防护：禁止客户端直接传递绝对路径，防止绕过沙箱
    if (arg[0] == '/') {
        return -1;
    }
    return 0;
}

/**
 * @brief  根据当前逻辑路径拼接出对应的服务器真实路径
 * @param  res 输出参数，用来保存最终真实路径
 * @param  size res 缓冲区大小
 * @param  current_path 当前逻辑路径
 * @return 成功返回 0，失败返回 -1
 */
int get_current_real_path(char *res, int size, const char *current_path) {
    const char *base_dir = get_server_base_dir();
    int ret = snprintf(res, size, "%s%s", base_dir, current_path);
    if (ret < 0 || ret >= size) {
        return -1;
    }
    return 0;
}

/**
 * @brief  根据当前逻辑路径和用户参数拼接服务器上的真实路径
 * @param  res 输出参数，用来保存最终真实路径
 * @param  size res 缓冲区大小
 * @param  path 当前逻辑路径
 * @param  arg 用户输入的目录名或文件名
 * @return 成功返回 0，失败返回 -1
 */
int get_real_path(char *res, int size, const char *path, const char *arg) {
    if (check_arg_path(arg) == -1) {
        return -1;
    }

    int ret = 0;
    const char *base_dir = get_server_base_dir();

    // 根目录需要单独处理，避免拼出双斜杠。
    if (strcmp(path, "/") == 0) {
        ret = snprintf(res, size, "%s/%s", base_dir, arg);
    } else {
        ret = snprintf(res, size, "%s%s/%s", base_dir, path, arg);
    }

    if (ret < 0 || ret >= size) {
        return -1;
    }
    return 0;
}

/**
 * @brief  根据当前逻辑路径和目标名字更新 current_path
 * @param  current_path 输入输出参数，保存并更新当前逻辑路径
 * @param  size current_path 缓冲区大小
 * @param  arg 用户输入的目录名
 * @return 成功返回 0，失败返回 -1
 */
int update_current_path(char *current_path, int size, const char *arg) {
    char new_path[512] = {0};
    int ret = 0;

    // 根目录和非根目录的拼接规则不同：
    // 1. 根目录下进入子目录 -> /child
    // 2. 非根目录下进入子目录 -> /parent/child
    if (strcmp(current_path, "/") == 0) {
        ret = snprintf(new_path, sizeof(new_path), "/%s", arg);
    } else {
        ret = snprintf(new_path, sizeof(new_path), "%s/%s", current_path, arg);
    }

    if (ret < 0 || ret >= size) {
        return -1;
    }

    strcpy(current_path, new_path);
    return 0;
}
