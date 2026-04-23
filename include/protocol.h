#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <sys/types.h>

/* 普通命令参数统一使用固定长度数组，便于双方按固定结构体长度收发。 */
#define CMD_DATA_LEN 256

/* 文件名同样使用固定长度数组，避免协议层出现动态长度处理。 */
#define FILE_NAME_LEN 256

/* token 字符串长度上限。当前第四期实现中，512 字节已经足够。 */
#define TOKEN_LEN 512

/* 虚拟文件系统中单个名字的长度上限，客户端和服务端共用这套限制。 */
#define MAX_VFS_NAME_LEN 30

/* 用户会话上下文，目录切换和文件传输都依赖这里保存的状态。 */
typedef struct{
    int user_id;                 /* 用户 ID，登录成功后才有有效值 */
    char current_path[256];      /* 当前虚拟路径 */
    int current_dir_id;          /* 当前目录节点 ID，根目录约定为 0 */
}ClientContext;


/* 命令类型枚举，客户端和服务端通过该编号区分具体命令。 */
typedef enum {
    CMD_TYPE_INVALID = 0, // 非法命令，或者暂时无法识别的命令
    CMD_TYPE_PWD,         // 查看当前虚拟路径
    CMD_TYPE_CD,          // 切换目录
    CMD_TYPE_LS,          // 查看目录内容
    CMD_TYPE_GETS,        // 下载文件
    CMD_TYPE_PUTS,        // 上传文件
    CMD_TYPE_TOUCH,       // 创建文件
    CMD_TYPE_RM,          // 删除文件
    CMD_TYPE_MKDIR,       // 创建目录
    CMD_TYPE_RMDIR,       // 删除目录
    CMD_TYPE_REPLY,       // 服务端返回的普通文本响应
    CMD_TYPE_LOGIN,       // 登录命令
    CMD_TYPE_REGISTER,    // 注册命令
    CMD_TYPE_AUTH,        // 传输连接认证命令
    CMD_TYPE_TOKEN,       // 服务端返回 token 的命令
} cmd_type_t;

/* 普通命令结构体，既用于发送普通命令，也用于返回文本响应。 */
typedef struct {
    int cmd_type;              // 命令类型，对应上面的 cmd_type_t
    int data_len;              // data 里真正有效的字符串长度
    char data[CMD_DATA_LEN];   // 命令参数，或者普通文本响应
} command_packet_t;

/* 文件传输结构体，上传和下载共用这一套元数据。 */
typedef struct {
    int cmd_type;                  // 当前是 puts 还是 gets
    int data_len;                  // file_name 中有效的字符串长度
    off_t file_size;               // 文件总大小
    off_t offset;                  // 断点续传位置
    char file_name[FILE_NAME_LEN]; // 文件名
    char hash[65];                 // 文件内容的 sha256 哈希值，64 字节 + 1 字节 '\0'
} file_packet_t;

/* 传输连接认证结构体，独立传输连接建立后首先发送这一包。 */
typedef struct {
    int cmd_type;                      // 固定为 CMD_TYPE_AUTH
    int transfer_cmd;                  // 本次真实要执行的命令，通常是 puts 或 gets
    int data_len;                      // current_path 中有效字符串长度
    char current_path[CMD_DATA_LEN];   // 客户端主连接当前所在虚拟路径
    char token[TOKEN_LEN];             // 登录成功后服务端签发的 token
} auth_packet_t;

/* token 返回结构体，登录成功后服务端会额外发送给客户端保存。 */
typedef struct {
    int cmd_type;                  // 固定为 CMD_TYPE_TOKEN
    int is_ok;                     // 1 表示有有效 token，0 表示没有
    int data_len;                  // token 中有效字符串长度
    char token[TOKEN_LEN];         // token 正文
} token_packet_t;

/**
 * @brief  把命令字符串转换成命令枚举值
 * @param  cmd_str 命令字符串，例如 "pwd"、"ls"、"puts"
 * @return 成功时返回对应的 cmd_type_t，失败返回 CMD_TYPE_INVALID
 */
cmd_type_t get_cmd_type(const char *cmd_str);

/**
 * @brief  保证把指定字节数完整发送出去
 * @param  fd socket 文件描述符
 * @param  buf 要发送的数据起始地址
 * @param  len 本次总共要发送多少字节
 * @return 成功返回 0，失败返回 -1
 */
int send_full(int fd, const void *buf, int len);

/**
 * @brief  保证从 fd 中完整接收指定字节数
 * @param  fd socket 文件描述符
 * @param  buf 接收缓冲区起始地址
 * @param  len 本次必须收满的字节数
 * @return 成功时返回实际接收到的字节数，也就是 len，失败或对端断开时返回 <= 0
 */
int recv_full(int fd, void *buf, int len);

/**
 * @brief  初始化普通命令结构体
 * @param  packet 要被填写的结构体地址
 * @param  type 命令类型
 * @param  data 参数字符串或者响应字符串，可以传 NULL
 * @return 无
 */
void init_command_packet(command_packet_t *packet, cmd_type_t type, const char *data);

/**
 * @brief  初始化文件传输结构体
 * @param  packet 要被填写的结构体地址
 * @param  type 命令类型，一般是 CMD_TYPE_PUTS 或 CMD_TYPE_GETS
 * @param  file_name 文件名，可以传 NULL
 * @param  file_size 文件总大小
 * @param  offset 断点续传位置
 * @param  hash 文件的 SHA256 哈希值，可以传 NULL
 * @return 无
 */
void init_file_packet(file_packet_t *packet, cmd_type_t type, const char *file_name, off_t file_size, off_t offset, const char *hash);

/**
 * @brief  发送一个完整的普通命令结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_command_packet(int fd, const command_packet_t *packet);

/**
 * @brief  接收一个完整的普通命令结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_command_packet(int fd, command_packet_t *packet);

/**
 * @brief  发送一个完整的文件传输结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_file_packet(int fd, const file_packet_t *packet);

/**
 * @brief  接收一个完整的文件传输结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_file_packet(int fd, file_packet_t *packet);

/**
 * @brief  初始化传输连接认证结构体
 * @param  packet 要被填写的结构体地址
 * @param  transfer_cmd 本次真实要执行的命令类型
 * @param  current_path 当前虚拟路径
 * @param  token token 字符串
 * @return 无
 */
void init_auth_packet(auth_packet_t *packet, cmd_type_t transfer_cmd, const char *current_path, const char *token);

/**
 * @brief  发送一个完整的传输连接认证结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_auth_packet(int fd, const auth_packet_t *packet);

/**
 * @brief  接收一个完整的传输连接认证结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_auth_packet(int fd, auth_packet_t *packet);

/**
 * @brief  初始化 token 返回结构体
 * @param  packet 要被填写的结构体地址
 * @param  token token 字符串，可以传 NULL
 * @param  is_ok 是否有效，1 表示有效，0 表示无效
 * @return 无
 */
void init_token_packet(token_packet_t *packet, const char *token, int is_ok);

/**
 * @brief  发送一个完整的 token 返回结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_token_packet(int fd, const token_packet_t *packet);

/**
 * @brief  接收一个完整的 token 返回结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_token_packet(int fd, token_packet_t *packet);

#endif
