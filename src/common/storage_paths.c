#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "storage_paths.h"

#define PROJECT_TEST_BASE_DIR "../test"
#define DEFAULT_CLIENT_FILE_DIR "client_files"
#define DEFAULT_SERVER_FILE_DIR "server_files"

/**
 * @brief  把 test 根目录和子目录名字拼成完整路径
 * @param  dir 输出参数，用来保存最终目录路径
 * @param  size dir 缓冲区大小
 * @param  subdir_name test 目录下的子目录名字
 * @return 成功返回 0，失败返回 -1
 */
static int build_test_subdir_path(char *dir, size_t size, const char *subdir_name) {
    char base_dir[1024] = {0};

    if (dir == NULL || size == 0 || subdir_name == NULL || subdir_name[0] == '\0') {
        return -1;
    }

    if (get_test_base_dir_path(base_dir, sizeof(base_dir)) != 0) {
        return -1;
    }

    if (snprintf(dir, size, "%s/%s", base_dir, subdir_name) >= (int)size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  根据运行目录定位工程中的 test 根目录
 * @param  dir 输出参数，用来保存最终 test 目录路径
 * @param  size dir 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_test_base_dir_path(char *dir, size_t size) {
    if (dir == NULL || size == 0) {
        return -1;
    }

    // 工程可能从项目根目录启动，也可能从 bin 目录启动。
    // 这里统一把 test 根目录定位出来，后面客户端和服务端都复用这套规则。
    if (access(PROJECT_TEST_BASE_DIR, F_OK) == 0) {
        if (snprintf(dir, size, "%s", PROJECT_TEST_BASE_DIR) >= (int)size) {
            return -1;
        }
        return 0;
    }

    if (access("./test", F_OK) == 0) {
        if (snprintf(dir, size, "%s", "./test") >= (int)size) {
            return -1;
        }
        return 0;
    }

    if (snprintf(dir, size, "%s", PROJECT_TEST_BASE_DIR) >= (int)size) {
        return -1;
    }
    return 0;
}

/**
 * @brief  根据配置项名字拼接客户端本地文件目录路径
 * @param  dir 输出参数，用来保存最终客户端文件目录路径
 * @param  size dir 缓冲区大小
 * @param  dir_name 配置中读取到的目录名字，例如 client_files
 * @return 成功返回 0，失败返回 -1
 */
int get_client_file_dir_path(char *dir, size_t size, const char *dir_name) {
    const char *real_dir_name = dir_name;

    if (real_dir_name == NULL || real_dir_name[0] == '\0') {
        real_dir_name = DEFAULT_CLIENT_FILE_DIR;
    }

    return build_test_subdir_path(dir, size, real_dir_name);
}

/**
 * @brief  根据配置项名字拼接服务端真实文件目录路径
 * @param  dir 输出参数，用来保存最终服务端真实文件目录路径
 * @param  size dir 缓冲区大小
 * @param  dir_name 配置中读取到的目录名字，例如 server_files
 * @return 成功返回 0，失败返回 -1
 */
int get_server_file_dir_path(char *dir, size_t size, const char *dir_name) {
    const char *real_dir_name = dir_name;

    if (real_dir_name == NULL || real_dir_name[0] == '\0') {
        real_dir_name = DEFAULT_SERVER_FILE_DIR;
    }

    return build_test_subdir_path(dir, size, real_dir_name);
}

/**
 * @brief  确保指定目录存在，不存在时尝试创建
 * @param  dir 目录路径
 * @return 成功返回 0，失败返回 -1
 */
int ensure_storage_dir_exists(const char *dir) {
    struct stat st;

    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }

    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    if (mkdir(dir, 0777) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}
