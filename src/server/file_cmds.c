#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "file_cmds.h"
#include "dao_vfs.h"
#include "dao_file.h"
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "path_utils.h"

#define EMPTY_FILE_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define FILE_STORE_DIR_NAME "files"

/**
 * @brief  根据当前逻辑路径和用户参数拼接逻辑全路径
 * @param  current_path 当前会话所在逻辑路径
 * @param  arg 用户输入的目录名或文件名
 * @param  result 输出参数，用来保存最终逻辑全路径
 * @return 无
 */
static void build_abs_path(const char *current_path, const char *arg, char *result) {
    if (arg[0] == '/') { // 输入的是绝对路径
        strcpy(result, arg);
        return;
    }
    if (strcmp(current_path, "/") == 0) {
        sprintf(result, "/%s", arg);
    } else {
        sprintf(result, "%s/%s", current_path, arg);
    }
}

/**
 * @brief  根据当前逻辑路径计算父目录逻辑路径
 * @param  current_path 当前逻辑路径
 * @param  result 输出参数，用来保存父目录逻辑路径
 * @return 无
 */
static void get_parent_path(const char *current_path, char *result) {
    if (strcmp(current_path, "/") == 0) {
        strcpy(result, "/");
        return;
    }
    strcpy(result, current_path);
    char *last_slash = strrchr(result, '/');
    if (last_slash == result) { // 类似 "/abc" -> 返回 "/"
        strcpy(result, "/");
    } else if (last_slash != NULL) { // 类似 "/abc/def" -> 返回 "/abc"
        *last_slash = '\0';
    }
}

/**
 * @brief  从逻辑全路径中提取最后一级名字
 * @param  full_path 逻辑全路径
 * @param  file_name 输出参数，用来保存最后一级目录名或文件名
 * @return 无
 */
static void extract_file_name(const char *full_path, char *file_name) {
    const char *last_slash = strrchr(full_path, '/');
    if (last_slash != NULL) {
        strcpy(file_name, last_slash + 1);
    } else {
        strcpy(file_name, full_path);
    }
}

/**
 * @brief  检查虚拟文件系统中的名字是否合法
 * @param  file_name 待校验的目录名或文件名
 * @return 合法返回 1，不合法返回 0
 */
static int is_valid_vfs_name(const char *file_name) {
    size_t len = strlen(file_name);

    if (len == 0) {
        return 0;
    }

    if (len > MAX_VFS_NAME_LEN) {
        return 0;
    }

    return 1;
}

/**
 * @brief  获取服务端真实文件仓库根目录
 * @return 成功时返回可用目录字符串，失败时退回默认 SERVER_BASE_DIR
 */
static const char *get_server_base_dir(void) {
    // 服务端既可能从项目根目录启动，也可能从 bin 目录启动。
    // 这里动态探测真实存在的 test 目录，避免后续把真实文件落到错误位置。
    if (access(SERVER_BASE_DIR, F_OK) == 0) {
        return SERVER_BASE_DIR;
    }
    if (access("./test", F_OK) == 0) {
        return "./test";
    }
    return SERVER_BASE_DIR;
}

/**
 * @brief  确保真实文件仓库目录 test/files 存在
 * @param  store_dir 输出参数，用来保存最终真实文件仓库路径
 * @param  size store_dir 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int ensure_store_dir(char *store_dir, int size) {
    struct stat st;
    const char *base_dir = get_server_base_dir();

    // 这个目录保存的是“真实文件实体”，而不是用户可见的逻辑目录结构。
    // paths 表中的目录树和这里完全解耦。
    if (snprintf(store_dir, size, "%s/%s", base_dir, FILE_STORE_DIR_NAME) >= size) {
        return -1;
    }

    if (stat(store_dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    if (mkdir(store_dir, 0777) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/**
 * @brief  根据 sha256 值拼接真实文件完整路径
 * @param  real_path 输出参数，用来保存真实文件完整路径
 * @param  size real_path 缓冲区大小
 * @param  sha256sum 64 位十六进制 SHA-256 字符串
 * @return 成功返回 0，失败返回 -1
 */
static int build_store_file_path(char *real_path, int size, const char *sha256sum) {
    char store_dir[MAX_PATH_LEN] = {0};

    if (ensure_store_dir(store_dir, sizeof(store_dir)) != 0) {
        return -1;
    }

    if (snprintf(real_path, size, "%s/%s", store_dir, sha256sum) >= size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  在删除逻辑文件后，按引用计数回收真实文件实体
 * @param  file_id files 表中的真实文件 id
 * @return 成功返回 0，失败返回 -1
 */
static int release_file_entity_if_unused(int file_id) {
    char sha256sum[65] = {0};
    off_t file_size = 0;
    int ref_count = 0;
    char real_path[MAX_PATH_LEN] = {0};

    // 第一步：先取出真实文件 hash。
    // 删除逻辑节点后，如果引用计数归零，服务端就需要根据这个 hash 去定位 test/files/<sha256>。
    if (dao_file_get_info_by_id(file_id, sha256sum, &file_size) != 0) {
        return -1;
    }

    // 第二步：把 files.count 减 1。
    // 这一步表示“少了一个逻辑文件节点引用这份真实实体”。
    if (dao_file_sub_ref_count(file_id) != 0) {
        return -1;
    }

    // 第三步：再查一次最新 count，判断这份真实文件是否还有其它逻辑节点在共享。
    if (dao_file_get_ref_count(file_id, &ref_count) != 0) {
        return -1;
    }

    // 只要 count 还大于 0，说明还有别的用户/别的逻辑路径引用这份实体，
    // 此时绝对不能删除真实文件，否则会把其它逻辑文件一起删坏。
    if (ref_count > 0) {
        return 0;
    }

    // 只有 count 归零，才说明这份真实文件已经成为“孤儿实体”，可以安全回收。
    if (build_store_file_path(real_path, sizeof(real_path), sha256sum) == 0) {
        if (unlink(real_path) == -1 && errno != ENOENT) {
            LOG_WARN("回收真实文件失败，file_id=%d，路径=%s，错误码=%d", file_id, real_path, errno);
        }
    }

    // 最后删除 files 表记录，让数据库元数据与磁盘状态保持一致。
    return dao_file_delete(file_id);
}

/**
 * @brief  确保空文件对应的真实文件实体存在
 * @return 成功返回 0，失败返回 -1
 */
static int ensure_empty_store_file_exists(void) {
    char real_path[MAX_PATH_LEN] = {0};

    if (build_store_file_path(real_path, sizeof(real_path), EMPTY_FILE_SHA256) != 0) {
        return -1;
    }

    // 空文件是 touch 共享复用的一份特殊真实实体。
    // 它在物理层对应一个 0 字节文件，文件名固定为“空内容的 SHA-256”。
    int fd = open(real_path, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        return -1;
    }

    close(fd);
    return 0;
}

/**
 * @brief  为 touch 命令创建空文件逻辑节点，并关联共享空文件实体
 * @param  ctx 当前客户端会话上下文
 * @param  target_path 目标逻辑全路径
 * @param  file_name 最后一级文件名
 * @return 成功返回 0，失败返回 -1
 */
static int create_empty_file_node(ClientContext *ctx, const char *target_path, const char *file_name) {
    int file_id = 0;
    off_t file_size = 0;
    int inserted_new_file = 0;

    // 第一步：确保磁盘上的空文件实体存在。
    // 否则就算 files 表里有空文件记录，后续 gets 也找不到真实文件。
    if (ensure_empty_store_file_exists() != 0) {
        return -1;
    }

    // 第二步：如果 files 表里已经存在空文件实体，就直接复用它。
    if (dao_file_find_by_sha256(EMPTY_FILE_SHA256, &file_id, &file_size) == 0) {
        if (dao_create_file_node(ctx->user_id, target_path, ctx->current_dir_id, file_name, file_id) != 0) {
            return -1;
        }

        // 新逻辑节点复用了已有真实文件，因此引用计数必须加 1。
        return dao_file_add_ref_count(file_id);
    }

    // 第三步：如果 files 表里还没有空文件记录，就插入一条新的真实文件记录。
    if (dao_file_insert(EMPTY_FILE_SHA256, 0, &file_id) != 0) {
        // 极少数并发场景下，可能是别的线程刚好先一步插入成功了。
        // 这时退化成“再查一次 -> 复用 -> count+1”即可。
        if (dao_file_find_by_sha256(EMPTY_FILE_SHA256, &file_id, &file_size) != 0) {
            return -1;
        }

        if (dao_create_file_node(ctx->user_id, target_path, ctx->current_dir_id, file_name, file_id) != 0) {
            return -1;
        }

        return dao_file_add_ref_count(file_id);
    }

    inserted_new_file = 1;
    if (dao_create_file_node(ctx->user_id, target_path, ctx->current_dir_id, file_name, file_id) == 0) {
        return 0;
    }

    // 如果 files 表是当前线程新插入的，但 paths 节点创建失败了，
    // 就要把这条孤立的 files 记录删掉，避免留下无人引用的空实体元数据。
    if (inserted_new_file) {
        dao_file_delete(file_id);
    }
    return -1;
}

// ================== 业务命令实现 ==================

void handle_pwd(int client_fd, ClientContext *ctx) {
    LOG_INFO("客户端请求 pwd，fd=%d，当前路径=%s", client_fd, ctx->current_path);
    send_msg(client_fd, ctx->current_path);
}

/**
 * @brief  处理 ls 命令，列出当前逻辑目录下的内容
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @return 无
 */
void handle_ls(int client_fd, ClientContext *ctx) {
    char buf[4096] = {0};
    int ret = dao_list_dir(ctx->user_id, ctx->current_dir_id, buf);
    
    if (ret < 0) {
        LOG_WARN("客户端请求ls失败，客户端fd=%d，当前路径=%s，错误码=%d", client_fd, ctx->current_path, ret);
        send_msg(client_fd, "服务器查询目录失败");
    }else if(ret==0){
        LOG_INFO("客户端请求ls，目录为空，客户端fd=%d，当前路径=%s", client_fd, ctx->current_path);
        send_msg(client_fd, "当前目录为空");
    }else{
        LOG_INFO("客户端请求ls成功，客户端fd=%d，当前路径=%s，返回条目数=%d", client_fd, ctx->current_path, ret);
        send_msg(client_fd, buf);
    }
}

/**
 * @brief  处理 cd 命令，切换当前会话所在逻辑目录
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的目录参数
 * @return 无
 */
void handle_cd(int client_fd, ClientContext *ctx, char *arg) {
    if (arg == NULL || arg[0] == '\0') {
        send_msg(client_fd, "cd 命令缺少参数");
        return;
    }

    char target_path[256] = {0};

    // cd .. 表示回到当前目录的父目录。
    if (strcmp(arg, "..") == 0) {
        get_parent_path(ctx->current_path, target_path);
    } 
    // cd . 表示留在当前目录，不需要改上下文。
    else if (strcmp(arg, ".") == 0) {
        send_msg(client_fd, "进入目录成功");
        return;
    } 
    // 其它情况按“普通目录名或绝对路径”处理。
    else {
        build_abs_path(ctx->current_path, arg, target_path);
    }

    // 查库判断目标路径是否存在，且必须是目录 (type=1)
    int target_id, node_type;
    if (dao_get_node_by_path(ctx->user_id, target_path, &target_id, &node_type) != 0) {
        send_msg(client_fd, "错误：目录不存在");
        return;
    }

    if (node_type != 1) {
        send_msg(client_fd, "错误：目标不是一个目录");
        return;
    }

    // 只有数据库确认目标路径存在且确实是目录，才更新会话上下文。
    // current_path 负责给用户展示当前位置；
    // current_dir_id 负责后续 ls / mkdir / touch / puts 等命令在数据库里定位当前目录节点。
    strcpy(ctx->current_path, target_path);
    ctx->current_dir_id = target_id;
    
    LOG_INFO("cd 成功，当前上下文：用户=%d, 路径=%s, current_dir_id=%d", ctx->user_id, ctx->current_path, ctx->current_dir_id);
    send_msg(client_fd, "进入目录成功");
}

/**
 * @brief  处理 mkdir 命令，在当前目录下创建子目录
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的目录名
 * @return 无
 */
void handle_mkdir(int client_fd, ClientContext *ctx, char *arg) {
    char target_path[256] = {0};
    char file_name[64] = {0};

    if (arg == NULL || arg[0] == '\0') {
        send_msg(client_fd, "mkdir 命令缺少参数");
        return;
    }
    
    build_abs_path(ctx->current_path, arg, target_path);
    extract_file_name(target_path, file_name);

    // 目录名长度限制必须在进数据库前拦截。
    // 否则可能出现服务端前面已经做了部分处理，最后才在 SQL 层失败的脏状态。
    if (!is_valid_vfs_name(file_name)) {
        send_msg(client_fd, "错误：文件名或目录名过长，最大长度为 30");
        return;
    }

    int tmp_id, tmp_type;
    if (dao_get_node_by_path(ctx->user_id, target_path, &tmp_id, &tmp_type) == 0) {
        send_msg(client_fd, "错误：该文件或目录已存在");
        return;
    }

    // type=1 表示这是一个目录节点。
    if (dao_create_node(ctx->user_id, target_path, ctx->current_dir_id, file_name, 1) == 0) {
        send_msg(client_fd, "创建文件夹成功");
    } else {
        LOG_WARN("mkdir命令创建目录失败，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "创建文件夹失败，服务器内部错误");
    }
}

/**
 * @brief  处理 touch 命令，在当前目录下创建空文件
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_touch(int client_fd, ClientContext *ctx, char *arg) {
    char target_path[256] = {0};
    char file_name[64] = {0};

    if (arg == NULL || arg[0] == '\0') {
        send_msg(client_fd, "touch 命令缺少参数");
        return;
    }
    
    build_abs_path(ctx->current_path, arg, target_path);
    extract_file_name(target_path, file_name);

    if (!is_valid_vfs_name(file_name)) {
        send_msg(client_fd, "错误：文件名或目录名过长，最大长度为 30");
        return;
    }

    int tmp_id, tmp_type;
    if (dao_get_node_by_path(ctx->user_id, target_path, &tmp_id, &tmp_type) == 0) {
        send_msg(client_fd, "错误：该文件或目录已存在");
        return;
    }

    // touch 创建的是“空文件逻辑节点 + 共享空文件实体”。
    // 这样 paths.file_id 与 files 表会保持一致，后续 gets 也能正常下载。
    // touch 不是只在 paths 表里塞一条“空壳文件节点”。
    // 它还必须保证 files 表和真实文件仓库里存在可下载的空文件实体，
    // 否则后续 gets 会出现“逻辑上有文件，物理上找不到”的不一致。
    if (create_empty_file_node(ctx, target_path, file_name) == 0) {
        LOG_INFO("touch命令创建文件成功，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "创建文件成功");
    } else {
        LOG_WARN("touch命令创建文件失败，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "创建文件失败，服务器内部错误");
    }
}

/**
 * @brief  处理 rm 命令，删除逻辑文件并在需要时回收真实文件实体
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的文件名
 * @return 无
 */
void handle_rm(int client_fd, ClientContext *ctx, char *arg) {
    char target_path[256] = {0};
    int file_node_id = 0;
    int file_id = 0;

    if (arg == NULL || arg[0] == '\0') {
        send_msg(client_fd, "rm 命令缺少参数");
        return;
    }

    build_abs_path(ctx->current_path, arg, target_path);

    int target_id, node_type;
    if (dao_get_node_by_path(ctx->user_id, target_path, &target_id, &node_type) != 0) {
        send_msg(client_fd, "错误：文件不存在");
        return;
    }

    if (node_type == 1) {
        send_msg(client_fd, "错误：目标是目录，请使用 rmdir 命令");
        return;
    }

    // 删除前先查出 file_id。
    // 因为 paths 节点一旦删掉，就无法再通过逻辑路径反查对应真实文件实体了。
    if (dao_get_file_info_by_path(ctx->user_id, target_path, &file_node_id, &file_id) != 0) {
        LOG_WARN("rm命令查询文件实体失败，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "文件删除失败，无法定位真实文件");
        return;
    }

    // 删除逻辑文件要分成两步：
    // 1. 删除 paths 中的逻辑节点
    // 2. 按引用计数决定是否回收 files 记录和真实文件
    // 只有两步都成功，这次 rm 才算真正完成。
    if (dao_delete_node(ctx->user_id, target_id) == 0 && release_file_entity_if_unused(file_id) == 0) {
        send_msg(client_fd, "文件删除成功");
    } else {
        LOG_WARN("rm命令删除文件失败，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "文件删除失败，数据库异常");
    }
}

/**
 * @brief  处理 rmdir 命令，删除当前目录下的空目录
 * @param  client_fd 当前客户端套接字
 * @param  ctx 当前客户端会话上下文
 * @param  arg 用户输入的目录名
 * @return 无
 */
void handle_rmdir(int client_fd, ClientContext *ctx, char *arg) {
    char target_path[256] = {0};

    if (arg == NULL || arg[0] == '\0') {
        send_msg(client_fd, "rmdir 命令缺少参数");
        return;
    }

    build_abs_path(ctx->current_path, arg, target_path);

    int target_id, node_type;
    if (dao_get_node_by_path(ctx->user_id, target_path, &target_id, &node_type) != 0) {
        send_msg(client_fd, "错误：目录不存在");
        return;
    }

    if (node_type == 0) {
        send_msg(client_fd, "错误：目标是文件，请使用 rm 命令");
        return;
    }

    // rmdir 只允许删除空目录。
    // 如果目录里还有内容，直接删除会把目录树结构破坏掉，因此必须先查空目录状态。
    int empty_status=dao_is_dir_empty(ctx->user_id,target_id);

    if(empty_status==-1){
        LOG_WARN("rmdir命令检查目录是否为空失败，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "服务器无法校验目录状态，无法删除");
        return;
    }
    if(empty_status==0){
        send_msg(client_fd, "目标目录非空，请先删除目录下的内容");
        return;
    }
    // 只有 empty_status == 1，也就是数据库确认“该目录没有子节点”时，才真正执行删除。
    if(dao_delete_node(ctx->user_id,target_id)==0){
        LOG_INFO("rmdir命令删除目录成功，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "删除目录成功");
    } else {
        LOG_WARN("rmdir命令删除目录失败，客户端fd=%d，目标路径=%s", client_fd, target_path);
        send_msg(client_fd, "删除目录失败,数据库异常");
    }
}
