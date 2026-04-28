#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <sys/types.h>

// 普通命令参数统一使用固定长度数组。
// 这样做的好处是协议大小固定，客户端和服务端都容易收发。
#define CMD_DATA_LEN 256

// 文件名也统一使用固定长度数组。
#define FILE_NAME_LEN 256

// 多点下载时，单次最多返回多少个可用数据源服务器。
// 第一版先把上限控制在 8，既够用，也方便把协议结构体做成固定大小。
#define MAX_SOURCE_SERVER_COUNT 8

// JWT 令牌使用固定长度字符数组保存。
// 这样客户端和服务端都可以直接按结构体完整收发。
#define TOKEN_LEN 1024

// 一次性传输票据也使用固定长度字符数组保存。
// 当前实现直接复用 JWT 字符串格式，所以长度和 TOKEN_LEN 保持一致。
#define TRANSFER_TICKET_LEN 1024

// 传输连接会直接携带完整虚拟路径。
// 路径字段单独给更大的空间，避免和普通命令参数混在一起。
#define FULL_PATH_LEN 512

// paths.file_name 在数据库中的上限是 30。
// 客户端和服务端共用这个限制，避免运行到 SQL 插入阶段才暴露错误。
#define MAX_VFS_NAME_LEN 30

// 用户会话上下文。
// 一个客户端连接对应一份独立的会话状态，
// 后续目录切换、上传下载都依赖这里保存的上下文信息。
typedef struct{
    int user_id;//用户 ID，登录后才有值
    char current_path[256];//当前虚拟路径
    int current_dir_id;//当前所在目录的节点 ID，根目录约定为 0
}ClientContext;

// 连接角色枚举。
// 新连接建立后，客户端会先告诉服务端：
// 这条连接是控制连接，还是传输连接。
typedef enum {
    CONN_ROLE_INVALID = 0,
    CONN_ROLE_CTRL = 1,
    CONN_ROLE_TRANSFER = 2,
} conn_role_t;


// 命令类型枚举。
// 客户端发命令时写入这个编号。
// 服务端收命令时读取这个编号。
// 这样双方不需要每次都重复传完整字符串去判断命令。
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
    CMD_TYPE_GETS_RANGE,  // 第五期内部使用：为某个下载分片申请区间票据
} cmd_type_t;

// 普通命令结构体。
// 这个结构体负责传输：
// 1. 普通命令的参数
// 2. 服务端返回的文本消息
typedef struct {
    int cmd_type;              // 命令类型，对应上面的 cmd_type_t
    int data_len;              // data 里真正有效的字符串长度
    char data[CMD_DATA_LEN];   // 命令参数，或者普通文本响应
} command_packet_t;

// 文件传输结构体。
// 上传和下载都需要知道文件名、总大小、断点位置。
// 所以把这些字段统一放进一个结构体里，双方协议就统一了。
typedef struct {
    int cmd_type;                  // 当前是 puts 还是 gets
    int data_len;                  // file_name 中有效的字符串长度
    off_t file_size;               // 文件总大小
    off_t offset;                  // 断点续传位置
    char file_name[FILE_NAME_LEN]; // 文件名
    char hash[65];                 // 文件内容的 sha256 哈希值，64 字节 + 1 字节 '\0'
} file_packet_t;

// 连接初始化结构体。
// 客户端每次新建连接后，第一件事就是发它。
// 服务端通过这里的 role 判断后续应该走哪条处理分支。
typedef struct {
    int role;                      // 连接角色，对应 conn_role_t
} conn_init_packet_t;

// 传输连接认证结构体。
// 客户端先在控制连接上申请一次性传输票据。
// 传输线程真正建立新连接后，只需要把这张票据发给服务端即可。
typedef struct {
    char ticket[TRANSFER_TICKET_LEN];  // 一次性传输票据
} transfer_auth_packet_t;

// 登录响应结构体。
// 登录成功时，服务端除了告诉客户端“登录成功”，
// 还需要把 user_id 和 JWT 一起返回。
typedef struct {
    int success;                   // 1 表示登录成功，0 表示失败
    int user_id;                   // 登录成功后的用户 id
    char message[CMD_DATA_LEN];    // 给客户端显示的提示信息
    char token[TOKEN_LEN];         // 登录成功后返回的 JWT
} auth_reply_packet_t;

// 一次性传输票据响应结构体。
// puts/gets 在控制连接上先申请票据，
// 服务端返回这类响应，客户端拿到成功结果后再启动传输线程。
typedef struct {
    int success;                               // 1 表示申请成功，0 表示失败
    char message[CMD_DATA_LEN];                // 给客户端显示的提示信息
    char ticket[TRANSFER_TICKET_LEN];          // 申请成功后返回的一次性传输票据
} transfer_ticket_reply_packet_t;

// 一个数据源服务器的信息。
// 第五期中，控制服务器会把可用的数据源列表返回给客户端。
typedef struct {
    char ip[64];     // 数据源服务器 IP
    char port[16];   // 数据源服务器端口
} source_server_t;

// 多点下载方案响应结构体。
// 客户端先通过控制连接申请方案，
// 服务端返回文件信息和可用数据源列表。
typedef struct {
    int success;                                        // 1 表示成功，0 表示失败
    off_t file_size;                                    // 目标文件总大小
    char file_name[FILE_NAME_LEN];                      // 目标文件名
    char file_hash[65];                                 // 目标文件 SHA-256
    int source_count;                                   // 当前返回了多少个可用数据源
    source_server_t sources[MAX_SOURCE_SERVER_COUNT];   // 数据源服务器列表
    char message[CMD_DATA_LEN];                         // 提示信息
} multi_gets_plan_packet_t;

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
 * @brief  初始化连接初始化结构体
 * @param  packet 要被填写的结构体地址
 * @param  role 连接角色
 * @return 无
 */
void init_conn_init_packet(conn_init_packet_t *packet, conn_role_t role);

/**
 * @brief  初始化传输认证结构体
 * @param  packet 要被填写的结构体地址
 * @param  ticket 一次性传输票据，可以传 NULL
 * @return 无
 */
void init_transfer_auth_packet(transfer_auth_packet_t *packet, const char *ticket);

/**
 * @brief  初始化登录响应结构体
 * @param  packet 要被填写的结构体地址
 * @param  success 是否成功
 * @param  user_id 登录成功后的用户 id
 * @param  message 提示信息，可以传 NULL
 * @param  token JWT 字符串，可以传 NULL
 * @return 无
 */
void init_auth_reply_packet(auth_reply_packet_t *packet, int success, int user_id,
                            const char *message, const char *token);

/**
 * @brief  初始化一次性传输票据响应结构体
 * @param  packet 要被填写的结构体地址
 * @param  success 是否成功
 * @param  message 提示信息，可以传 NULL
 * @param  ticket 一次性传输票据，可以传 NULL
 * @return 无
 */
void init_transfer_ticket_reply_packet(transfer_ticket_reply_packet_t *packet,
                                       int success, const char *message, const char *ticket);

/**
 * @brief  初始化多点下载方案响应结构体
 * @param  packet 要被填写的结构体地址
 * @param  success 是否成功
 * @param  file_name 目标文件名，可以传 NULL
 * @param  file_size 目标文件总大小
 * @param  file_hash 目标文件 SHA-256，可以传 NULL
 * @param  source_count 数据源数量
 * @param  message 提示信息，可以传 NULL
 * @return 无
 */
void init_multi_gets_plan_packet(multi_gets_plan_packet_t *packet, int success,
                                 const char *file_name, off_t file_size,
                                 const char *file_hash, int source_count,
                                 const char *message);

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
 * @brief  发送一个完整的连接初始化结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_conn_init_packet(int fd, const conn_init_packet_t *packet);

/**
 * @brief  接收一个完整的连接初始化结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_conn_init_packet(int fd, conn_init_packet_t *packet);

/**
 * @brief  发送一个完整的传输认证结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_transfer_auth_packet(int fd, const transfer_auth_packet_t *packet);

/**
 * @brief  接收一个完整的传输认证结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_transfer_auth_packet(int fd, transfer_auth_packet_t *packet);

/**
 * @brief  发送一个完整的登录响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_auth_reply_packet(int fd, const auth_reply_packet_t *packet);

/**
 * @brief  接收一个完整的登录响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_auth_reply_packet(int fd, auth_reply_packet_t *packet);

/**
 * @brief  发送一个完整的一次性传输票据响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_transfer_ticket_reply_packet(int fd, const transfer_ticket_reply_packet_t *packet);

/**
 * @brief  接收一个完整的一次性传输票据响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 用于保存结果的结构体地址
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_transfer_ticket_reply_packet(int fd, transfer_ticket_reply_packet_t *packet);

/**
 * @brief  发送一个完整的多点下载方案响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_multi_gets_plan_packet(int fd, const multi_gets_plan_packet_t *packet);

/**
 * @brief  接收一个完整的多点下载方案响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_multi_gets_plan_packet(int fd, multi_gets_plan_packet_t *packet);

#endif
