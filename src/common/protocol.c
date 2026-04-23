#include <string.h>
#include <sys/socket.h>
#include "protocol.h"

/**
 * @brief  把字符串形式的命令转换成命令枚举
 * @param  cmd_str 命令字符串，例如 "pwd"、"puts"
 * @return 成功时返回对应命令枚举，失败返回 CMD_TYPE_INVALID
 */
cmd_type_t get_cmd_type(const char *cmd_str) {
    // 第一步：先校验输入指针。
    if (cmd_str == NULL) {
        return CMD_TYPE_INVALID;
    }

    // 第二步：按字符串内容映射到内部命令枚举。
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
    if (strcmp(cmd_str, "auth") == 0) {
        return CMD_TYPE_AUTH;
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
    int total = 0;                    // 已成功发送的字节总数
    const char *p = (const char *)buf; // 转换为字节指针，便于按偏移继续发送

    // 循环发送，直到 len 字节全部发出。
    while (total < len) {
        // p + total 表示本轮发送的起始位置。
        int ret = send(fd, p + total, len - total, MSG_NOSIGNAL);

        // ret <= 0 表示发送失败，或连接已经异常关闭。
        if (ret <= 0) {
            return -1;
        }

        // 根据本轮实际发送长度推进 total。
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
    int total = 0;             // 已成功接收的字节总数
    char *p = (char *)buf;     // 转换为字节指针，便于按偏移写入缓冲区

    // 循环接收，直到 len 字节全部收满。
    while (total < len) {
        // p + total 表示本轮写入缓冲区的起始位置。
        int ret = recv(fd, p + total, len - total, 0);

        // ret <= 0 表示接收失败，或对端已经关闭连接。
        if (ret <= 0) {
            return ret;
        }

        // 根据本轮实际接收长度推进 total。
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
    // 第一步：先整体清零，避免结构体中保留旧内容。
    memset(packet, 0, sizeof(command_packet_t));

    // 第二步：记录命令类型。
    packet->cmd_type = type;

    // 第三步：data 非空时，写入命令参数或文本消息。
    if (data != NULL) {
        // 保留最后一个字节给 '\0'，确保 data 始终可作为字符串使用。
        strncpy(packet->data, data, CMD_DATA_LEN - 1);

        // data_len 记录当前字符串的有效长度。
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
    // 第一步：先整体清零，避免结构体中保留旧内容。
    memset(packet, 0, sizeof(file_packet_t));

    // 第二步：填写文件传输的基础元数据。
    packet->cmd_type = type;
    packet->file_size = file_size;
    packet->offset = offset;

    // 第三步：文件名非空时，复制文件名并记录长度。
    if (file_name != NULL) {
        strncpy(packet->file_name, file_name, FILE_NAME_LEN - 1);
        packet->data_len = strlen(packet->file_name);
    }

    // 第四步：hash 非空时，复制文件哈希值。
    if(hash != NULL) {
        strncpy(packet->hash, hash, 64);
        packet->hash[64] = '\0';
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
    // 第一步：按固定长度接收整个普通命令结构体。
    int ret = recv_full(fd, packet, sizeof(command_packet_t));
    if (ret <= 0) {
        return ret;
    }

    // 第二步：校验 data_len 是否在合法范围内。
    if (packet->data_len < 0 || packet->data_len >= CMD_DATA_LEN) {
        return -1;
    }

    // 第三步：补齐字符串结尾，保证后续字符串处理安全。
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
    // 第一步：按固定长度接收整个文件传输结构体。
    int ret = recv_full(fd, packet, sizeof(file_packet_t));
    if (ret <= 0) {
        return ret;
    }

    // 第二步：校验 file_name 对应的有效长度。
    if (packet->data_len < 0 || packet->data_len >= FILE_NAME_LEN) {
        return -1;
    }

    // 第三步：补齐字符串结尾，保证 file_name 可安全作为字符串使用。
    packet->file_name[FILE_NAME_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  初始化一个传输连接认证包
 * @param  packet 要写入的认证结构体地址
 * @param  transfer_cmd 本次真实要执行的命令类型
 * @param  current_path 当前虚拟路径
 * @param  token token 字符串
 * @return 无
 */
void init_auth_packet(auth_packet_t *packet, cmd_type_t transfer_cmd, const char *current_path, const char *token) {
    // 第一步：先整体清零。
    memset(packet, 0, sizeof(auth_packet_t));

    // 第二步：填写认证包的固定字段。
    packet->cmd_type = CMD_TYPE_AUTH;
    packet->transfer_cmd = transfer_cmd;

    // 第三步：复制当前路径，并记录路径长度。
    if (current_path != NULL) {
        strncpy(packet->current_path, current_path, CMD_DATA_LEN - 1);
        packet->data_len = strlen(packet->current_path);
    }

    // 第四步：复制 token 字符串。
    if (token != NULL) {
        strncpy(packet->token, token, TOKEN_LEN - 1);
    }
}

/**
 * @brief  发送一个完整的传输连接认证包
 * @param  fd socket 文件描述符
 * @param  packet 要发送的认证结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_auth_packet(int fd, const auth_packet_t *packet) {
    return send_full(fd, packet, sizeof(auth_packet_t));
}

/**
 * @brief  接收一个完整的传输连接认证包
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_auth_packet(int fd, auth_packet_t *packet) {
    // 第一步：按固定长度接收整个认证结构体。
    int ret = recv_full(fd, packet, sizeof(auth_packet_t));
    if (ret <= 0) {
        return ret;
    }

    // 第二步：校验 current_path 的有效长度。
    if (packet->data_len < 0 || packet->data_len >= CMD_DATA_LEN) {
        return -1;
    }

    // 第三步：补齐字符串结尾。
    packet->current_path[CMD_DATA_LEN - 1] = '\0';
    packet->token[TOKEN_LEN - 1] = '\0';
    return ret;
}

/**
 * @brief  初始化一个 token 返回包
 * @param  packet 要写入的 token 结构体地址
 * @param  token token 字符串
 * @param  is_ok 是否有效
 * @return 无
 */
void init_token_packet(token_packet_t *packet, const char *token, int is_ok) {
    // 第一步：先整体清零。
    memset(packet, 0, sizeof(token_packet_t));

    // 第二步：填写 token 包的固定字段。
    packet->cmd_type = CMD_TYPE_TOKEN;
    packet->is_ok = is_ok;

    // 第三步：token 非空时，复制 token 并记录长度。
    if (token != NULL) {
        strncpy(packet->token, token, TOKEN_LEN - 1);
        packet->data_len = strlen(packet->token);
    }
}

/**
 * @brief  发送一个完整的 token 返回包
 * @param  fd socket 文件描述符
 * @param  packet 要发送的 token 结构体地址
 * @return 成功返回 0，失败返回 -1
 */
int send_token_packet(int fd, const token_packet_t *packet) {
    return send_full(fd, packet, sizeof(token_packet_t));
}

/**
 * @brief  接收一个完整的 token 返回包
 * @param  fd socket 文件描述符
 * @param  packet 输出参数，用来保存接收结果
 * @return 成功时返回接收字节数，失败返回 <= 0 或 -1
 */
int recv_token_packet(int fd, token_packet_t *packet) {
    // 第一步：按固定长度接收整个 token 结构体。
    int ret = recv_full(fd, packet, sizeof(token_packet_t));
    if (ret <= 0) {
        return ret;
    }

    // 第二步：校验 token 字段的有效长度。
    if (packet->data_len < 0 || packet->data_len >= TOKEN_LEN) {
        return -1;
    }

    // 第三步：补齐 token 字符串结尾。
    packet->token[TOKEN_LEN - 1] = '\0';
    return ret;
}
