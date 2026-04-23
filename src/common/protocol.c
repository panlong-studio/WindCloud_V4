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
