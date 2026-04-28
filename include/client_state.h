#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include <pthread.h>
#include "protocol.h"
#include "path_utils.h"

// 客户端统一状态结构体。
// 主线程和传输线程都会访问它。
typedef struct {
    int ctrl_fd;                     // 控制连接 fd
    int logged_in;                   // 当前是否已经登录
    int user_id;                     // 当前登录用户 id
    char token[TOKEN_LEN];           // 登录成功后拿到的 JWT
    char current_path[256];          // 客户端当前认知的远端虚拟路径
    char server_ip[64];              // 服务端 IP
    char server_port[64];            // 服务端端口
    char client_file_dir[MAX_PATH_LEN]; // 客户端本地统一文件目录
    pthread_mutex_t lock;            // 保护共享状态
} ClientState;

#endif
