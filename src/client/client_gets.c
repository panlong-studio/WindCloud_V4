#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "client_command_handle.h"
#include "protocol.h"
#include "log.h"

#define BUFFER_SIZE 4096

/**
 * @brief  保证把一段数据完整写入本地文件
 * @param  fd 本地文件描述符
 * @param  buf 待写入数据起始地址
 * @param  len 本次需要写入的总字节数
 * @return 成功返回 0，失败返回 -1
 */
static int write_file_full(int fd, const char *buf, int len) {
    int total = 0;

    // 普通 write 可能一次只写入部分字节。
    // 下载时必须保证本轮收到的网络数据全部落盘，否则文件内容会错位。
    while (total < len) {
        int ret = write(fd, buf + total, len - total);
        if (ret <= 0) {
            return -1;
        }
        total += ret;
    }

    return 0;
}

/**
 * @brief  处理 gets 命令，支持普通下载与断点续传
 * @param  sock_fd 客户端套接字
 * @param  arg 用户输入的文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
int handle_gets_command(int sock_fd, const char *arg) {
    LOG_INFO("客户端请求下载文件，文件=%s", arg);

    // 第一步：先发送一个普通命令包，告诉服务端“我要下载哪个逻辑文件”。
    command_packet_t cmd_packet;
    init_command_packet(&cmd_packet, CMD_TYPE_GETS, arg);

    if (send_command_packet(sock_fd, &cmd_packet) == -1) {
        printf("发送命令失败\n");
        LOG_ERROR("发送下载命令失败，文件=%s", arg);
        return 0;
    }

    // 第二步：接收服务端回的文件信息包。
    // 这里会带回文件总大小，客户端据此决定是否续传。
    file_packet_t server_file_packet;
    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        printf("接收文件信息失败\n");
        LOG_WARN("接收下载文件信息失败，文件=%s", arg);
        return 0;
    }

    if (server_file_packet.file_size < 0) {
        printf("服务器文件不存在\n");
        LOG_WARN("服务端文件不存在，文件=%s", arg);
        return 0;
    }

    // 第三步：检查本地是否已经存在同名文件，并据此决定续传偏移。
    struct stat st;
    off_t local_size = 0;
    off_t request_offset = 0;
    int local_file_exists = 0;

    if (stat(arg, &st) == 0) {
        local_size = st.st_size;
        local_file_exists = 1;
    }

    if (local_size < server_file_packet.file_size) {
        request_offset = local_size;
    } else if (local_file_exists && local_size == server_file_packet.file_size) {
        request_offset = server_file_packet.file_size;
    } else {
        request_offset = 0;
    }

    LOG_DEBUG("下载断点信息，文件=%s，本地大小=%lld，服务端大小=%lld，偏移=%lld",
              arg,
              (long long)local_size,
              (long long)server_file_packet.file_size,
              (long long)request_offset);

    // 第四步：把“客户端本地已拥有的进度”回传给服务端。
    // 服务端后续会从这个偏移位置开始继续发送。
    file_packet_t client_file_packet;
    init_file_packet(&client_file_packet,
                     CMD_TYPE_GETS,
                     arg,
                     server_file_packet.file_size,
                     request_offset,
                     NULL);

    if (send_file_packet(sock_fd, &client_file_packet) == -1) {
        printf("发送续传位置失败\n");
        LOG_WARN("发送下载断点位置失败，文件=%s", arg);
        return -1;
    }

    if (local_file_exists && request_offset == server_file_packet.file_size) {
        printf("文件已存在且完整，无需下载。\n");
        LOG_INFO("下载已跳过，本地文件完整，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
        return 0;
    }

    // 第五步：准备本地文件句柄，真正开始接收文件正文。
    int fd = open(arg, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        perror("创建文件失败");
        LOG_ERROR("创建本地文件失败，文件=%s，错误码=%d", arg, errno);
        return -1;
    }

    if (request_offset == 0) {
        if (ftruncate(fd, 0) == -1) {
            perror("清空旧文件失败");
            LOG_ERROR("截断本地文件失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return -1;
        }
    }

    if (lseek(fd, request_offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地文件失败，文件=%s，偏移=%lld，错误码=%d",
                  arg,
                  (long long)request_offset,
                  errno);
        close(fd);
        return -1;
    }

    char buf[BUFFER_SIZE];

    // remaining 表示这次还需要从网络再收多少字节。
    off_t remaining = server_file_packet.file_size - request_offset;

    // 第六步：循环收网络数据并落盘，直到 remaining 归零。
    while (remaining > 0) {
        int once = BUFFER_SIZE;
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        if (recv_full(sock_fd, buf, once) <= 0) {
            printf("下载中断，已经保留当前进度。\n");
            LOG_WARN("下载中断，文件=%s，已保存=%lld，总大小=%lld",
                     arg,
                     (long long)(server_file_packet.file_size - remaining),
                     (long long)server_file_packet.file_size);
            close(fd);
            return 0;
        }

        if (write_file_full(fd, buf, once) == -1) {
            perror("写入本地文件失败");
            LOG_ERROR("写入本地文件失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return -1;
        }

        remaining -= once;
    }

    // 第七步：全部写完后关闭本地文件并提示下载成功。
    close(fd);
    printf("下载成功: %s (%ld 字节)\n", arg, (long)server_file_packet.file_size);
    LOG_INFO("下载成功，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
    return 0;
}
