#include <string.h>
#include <sys/socket.h>
#include "protocol.h"

/**
 * @brief  把字符串形式的命令转换成命令枚举
 * @param  cmd_str 命令字符串，例如 "pwd"、"puts"
 * @return 成功时返回对应命令枚举，失败返回 CMD_TYPE_INVALID
 */
cmd_type_t get_cmd_type(const char *cmd_str) {
    // 先做空指针判断，避免下面 strcmp 直接崩溃。
    if (cmd_str == NULL) {
        return CMD_TYPE_INVALID;
    }

    // strcmp 返回 0 说明两个字符串完全相等。
    if (strcmp(cmd_str, "pwd") == 0) {
        return CMD_TYPE_PWD;
    }
    if (strcmp(cmd_str, "cd") == 0) {
        return CMD_TYPE_CD;
    }
    if (strcmp(cmd_str, "ls") == 0) {
        return CMD_TYPE_LS;
    }
    if (strcmp(cmd_str, "gets") == 0) {
        return CMD_TYPE_GETS;
    }
    if (strcmp(cmd_str, "puts") == 0) {
        return CMD_TYPE_PUTS;
    }
    if(strcmp(cmd_str,"touch")==0){
        return CMD_TYPE_TOUCH;
    }
    // 当前项目统一只保留 rm 作为删除命令入口。
    // 这样可以避免 rm/remove 两套名字重复增加学习成本。
    if (strcmp(cmd_str, "rm") == 0) {
        return CMD_TYPE_RM;
    }
    if (strcmp(cmd_str, "mkdir") == 0) {
        return CMD_TYPE_MKDIR;
    }
    if(strcmp(cmd_str,"rmdir")==0){
        return CMD_TYPE_RMDIR;
    }
    if (strcmp(cmd_str, "login") == 0) {
        return CMD_TYPE_LOGIN;
    }
    if (strcmp(cmd_str, "register") == 0) {
        return CMD_TYPE_REGISTER;
    }

    return CMD_TYPE_INVALID;
}

/**
 * @brief  循环 send，直到把指定字节数完整发送出去
 * @param  fd socket 文件描述符
 * @param  buf 待发送数据起始地址
 * @param  len 总发送字节数
 * @return 成功返回 0，失败返回 -1
 */
int send_full(int fd, const void *buf, int len) {
    int total = 0;                    // 已经成功发送出去的总字节数
    const char *p = (const char *)buf; // 把无类型指针转成 char*，方便按字节偏移

    // 只要 total 还没到 len，就说明还有数据没发完。
    while (total < len) {
        // p + total 表示“从还没发出去的位置继续发”。
        int ret = send(fd, p + total, len - total, MSG_NOSIGNAL);

        // ret <= 0 说明发送失败，或者对端异常断开。
        if (ret <= 0) {
            return -1;
        }

        // ret 表示这一次实际发出去多少字节。
        total += ret;
    }

    return 0;
}

/**
 * @brief  循环 recv，直到把指定字节数完整接收回来
 * @param  fd socket 文件描述符
 * @param  buf 接收缓冲区起始地址
 * @param  len 必须收满的总字节数
 * @return 成功时返回 len，失败或对端关闭时返回 <= 0
 */
int recv_full(int fd, void *buf, int len) {
    int total = 0;             // 已经成功接收到的总字节数
    char *p = (char *)buf;     // 转成 char* 后，才能按字节移动指针

    // 只要还没收满，就继续收。
    while (total < len) {
        // p + total 表示“从缓冲区里尚未填充的位置继续写入”。
        int ret = recv(fd, p + total, len - total, 0);

        // ret <= 0 说明 recv 失败，或者对端关闭连接。
        if (ret <= 0) {
            return ret;
        }

        // 本轮收到了多少，就把 total 往后推进多少。
        total += ret;
    }

    return total;
}

/**
 * @brief  初始化一个普通命令包
 * @param  packet 要写入的普通命令结构体地址
 * @param  type 命令类型
 * @param  data 命令参数或普通文本消息
 * @return 无
 */
void init_command_packet(command_packet_t *packet, cmd_type_t type, const char *data) {
    // 先全部清零，避免旧数据残留。
    memset(packet, 0, sizeof(command_packet_t));

    // 写入命令类型。
    packet->cmd_type = type;

    // data 允许为空，所以这里先判断。
    if (data != NULL) {
        // strncpy 最多只拷贝 CMD_DATA_LEN - 1 个字节，
        // 最后一个字节留给 '\0'。
        strncpy(packet->data, data, CMD_DATA_LEN - 1);

        // data_len 记录真正有效的字符串长度。
        packet->data_len = strlen(packet->data);
    }
}

/**
 * @brief  初始化一个文件传输包
 * @param  packet 要写入的文件传输结构体地址
 * @param  type 命令类型
 * @param  file_name 文件名
 * @param  file_size 文件总大小
 * @param  offset 断点位置
 * @param  hash 文件内容哈希，可传 NULL
 * @return 无
 */
void init_file_packet(file_packet_t *packet, cmd_type_t type, const char *file_name, off_t file_size, off_t offset, const char *hash) {
    // 先把整个结构体清零，避免发送脏数据。
    memset(packet, 0, sizeof(file_packet_t));

    // 下面 3 行把最关键的文件信息写进去。
    packet->cmd_type = type;
    packet->file_size = file_size;
    packet->offset = offset;

    // 文件名可能为空，所以先判断。
    if (file_name != NULL) {
        // FILE_NAME_LEN - 1 的原因，和上面的普通命令结构体一样。
        strncpy(packet->file_name, file_name, FILE_NAME_LEN - 1);

        // 记录真实文件名长度。
        packet->data_len = strlen(packet->file_name);
    }

    if(hash != NULL) {
        strncpy(packet->hash, hash, 64);//拷贝 sha256 哈希值，64 字节，不包括 '\0'
        packet->hash[64] = '\0';
    }
}

/**
 * @brief  初始化连接初始化结构体
 * @param  packet 要写入的连接初始化结构体地址
 * @param  role 连接角色
 * @return 无
 */
void init_conn_init_packet(conn_init_packet_t *packet, conn_role_t role) {
    memset(packet, 0, sizeof(conn_init_packet_t));
    packet->role = role;
}

/**
 * @brief  初始化传输认证结构体
 * @param  packet 要写入的传输认证结构体地址
 * @param  ticket 一次性传输票据
 * @return 无
 */
void init_transfer_auth_packet(transfer_auth_packet_t *packet, const char *ticket) {
    memset(packet, 0, sizeof(transfer_auth_packet_t));

    if (ticket != NULL) {
        strncpy(packet->ticket, ticket, TRANSFER_TICKET_LEN - 1);
    }
}

/**
 * @brief  初始化登录响应结构体
 * @param  packet 要写入的登录响应结构体地址
 * @param  success 是否成功
 * @param  user_id 用户 id
 * @param  message 提示信息
 * @param  token JWT 字符串
 * @return 无
 */
void init_auth_reply_packet(auth_reply_packet_t *packet, int success, int user_id,
                            const char *message, const char *token) {
    memset(packet, 0, sizeof(auth_reply_packet_t));
    packet->success = success;
    packet->user_id = user_id;

    if (message != NULL) {
        strncpy(packet->message, message, CMD_DATA_LEN - 1);
    }

    if (token != NULL) {
        strncpy(packet->token, token, TOKEN_LEN - 1);
    }
}

/**
 * @brief  初始化一次性传输票据响应结构体
 * @param  packet 要写入的响应结构体地址
 * @param  success 是否成功
 * @param  message 提示信息
 * @param  ticket 一次性传输票据
 * @return 无
 */
void init_transfer_ticket_reply_packet(transfer_ticket_reply_packet_t *packet,
                                       int success, const char *message, const char *ticket) {
    memset(packet, 0, sizeof(transfer_ticket_reply_packet_t));
    packet->success = success;

    if (message != NULL) {
        strncpy(packet->message, message, CMD_DATA_LEN - 1);
    }

    if (ticket != NULL) {
        strncpy(packet->ticket, ticket, TRANSFER_TICKET_LEN - 1);
    }
}

/**
 * @brief  初始化多点下载方案响应结构体
 * @param  packet 要写入的响应结构体地址
 * @param  success 是否成功
 * @param  file_name 目标文件名
 * @param  file_size 目标文件总大小
 * @param  file_hash 目标文件 SHA-256
 * @param  source_count 当前可用数据源数量
 * @param  message 提示信息
 * @return 无
 */
void init_multi_gets_plan_packet(multi_gets_plan_packet_t *packet, int success,
                                 const char *file_name, off_t file_size,
                                 const char *file_hash, int source_count,
                                 const char *message) {
    memset(packet, 0, sizeof(multi_gets_plan_packet_t));
    packet->success = success;
    packet->file_size = file_size;
    packet->source_count = source_count;

    if (file_name != NULL) {
        strncpy(packet->file_name, file_name, FILE_NAME_LEN - 1);
    }

    if (file_hash != NULL) {
        strncpy(packet->file_hash, file_hash, 64);
        packet->file_hash[64] = '\0';
    }

    if (message != NULL) {
        strncpy(packet->message, message, CMD_DATA_LEN - 1);
    }
}

/**
 * @brief  发送一个完整的普通命令包
 * @param  fd socket 文件描述符
 * @param  packet 要发送的普通命令结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_command_packet(int fd, const command_packet_t *packet) {
    return send_full(fd, packet, sizeof(command_packet_t));
}

/**
 * @brief  接收一个完整的普通命令包
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_command_packet(int fd, command_packet_t *packet) {
    // 先按固定大小完整接收整个结构体。
    int ret = recv_full(fd, packet, sizeof(command_packet_t));
    if (ret <= 0) {
        return ret;
    }

    // 收到结构体后，再顺手检查一下 data_len 是否越界。
    if (packet->data_len < 0 || packet->data_len >= CMD_DATA_LEN) {
        return -1;
    }

    // 再手工补一个 '\0'，确保后面把 data 当字符串使用时安全。
    packet->data[CMD_DATA_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  发送一个完整的文件传输包
 * @param  fd socket 文件描述符
 * @param  packet 要发送的文件传输结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_file_packet(int fd, const file_packet_t *packet) {
    return send_full(fd, packet, sizeof(file_packet_t));
}

/**
 * @brief  接收一个完整的文件传输包
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_file_packet(int fd, file_packet_t *packet) {
    // 先把整个结构体完整收下来。
    int ret = recv_full(fd, packet, sizeof(file_packet_t));
    if (ret <= 0) {
        return ret;
    }

    // 再检查 file_name 的有效长度字段是否越界。
    if (packet->data_len < 0 || packet->data_len >= FILE_NAME_LEN) {
        return -1;
    }

    // 再补一个 '\0'，保证 file_name 一定能当字符串用。
    packet->file_name[FILE_NAME_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  发送一个完整的连接初始化结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_conn_init_packet(int fd, const conn_init_packet_t *packet) {
    return send_full(fd, packet, sizeof(conn_init_packet_t));
}

/**
 * @brief  接收一个完整的连接初始化结构体
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_conn_init_packet(int fd, conn_init_packet_t *packet) {
    return recv_full(fd, packet, sizeof(conn_init_packet_t));
}

/**
 * @brief  发送一个完整的传输认证结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_transfer_auth_packet(int fd, const transfer_auth_packet_t *packet) {
    return send_full(fd, packet, sizeof(transfer_auth_packet_t));
}

/**
 * @brief  接收一个完整的传输认证结构体
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_transfer_auth_packet(int fd, transfer_auth_packet_t *packet) {
    int ret = recv_full(fd, packet, sizeof(transfer_auth_packet_t));
    if (ret <= 0) {
        return ret;
    }

    packet->ticket[TRANSFER_TICKET_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  发送一个完整的登录响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_auth_reply_packet(int fd, const auth_reply_packet_t *packet) {
    return send_full(fd, packet, sizeof(auth_reply_packet_t));
}

/**
 * @brief  接收一个完整的登录响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_auth_reply_packet(int fd, auth_reply_packet_t *packet) {
    int ret = recv_full(fd, packet, sizeof(auth_reply_packet_t));
    if (ret <= 0) {
        return ret;
    }

    packet->message[CMD_DATA_LEN - 1] = '\0';
    packet->token[TOKEN_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  发送一个完整的一次性传输票据响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_transfer_ticket_reply_packet(int fd, const transfer_ticket_reply_packet_t *packet) {
    return send_full(fd, packet, sizeof(transfer_ticket_reply_packet_t));
}

/**
 * @brief  接收一个完整的一次性传输票据响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_transfer_ticket_reply_packet(int fd, transfer_ticket_reply_packet_t *packet) {
    int ret = recv_full(fd, packet, sizeof(transfer_ticket_reply_packet_t));
    if (ret <= 0) {
        return ret;
    }

    packet->message[CMD_DATA_LEN - 1] = '\0';
    packet->ticket[TRANSFER_TICKET_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  发送一个完整的多点下载方案响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 要发送的结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_multi_gets_plan_packet(int fd, const multi_gets_plan_packet_t *packet) {
    return send_full(fd, packet, sizeof(multi_gets_plan_packet_t));
}

/**
 * @brief  接收一个完整的多点下载方案响应结构体
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_multi_gets_plan_packet(int fd, multi_gets_plan_packet_t *packet) {
    int ret = recv_full(fd, packet, sizeof(multi_gets_plan_packet_t));
    int i = 0;

    if (ret <= 0) {
        return ret;
    }

    if (packet->source_count < 0 || packet->source_count > MAX_SOURCE_SERVER_COUNT) {
        return -1;
    }

    packet->file_name[FILE_NAME_LEN - 1] = '\0';
    packet->file_hash[64] = '\0';
    packet->message[CMD_DATA_LEN - 1] = '\0';

    for (i = 0; i < MAX_SOURCE_SERVER_COUNT; ++i) {
        packet->sources[i].ip[sizeof(packet->sources[i].ip) - 1] = '\0';
        packet->sources[i].port[sizeof(packet->sources[i].port) - 1] = '\0';
    }

    return ret;
}
