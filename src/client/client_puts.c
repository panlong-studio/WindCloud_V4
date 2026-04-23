#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "client_command_handle.h"
#include "protocol.h"
#include "log.h"
#include "sha256_utils.h"

#define BUFFER_SIZE 4096

/**
 * @brief  处理 puts 命令，支持普通上传、断点续传和秒传
 * @param  sock_fd 客户端套接字
 * @param  arg 用户输入的本地文件名
 * @return 成功返回 0，失败返回 -1 或 0
 */
int handle_puts_command(int sock_fd, const char *arg) {
    LOG_INFO("客户端请求上传文件，文件=%s", arg);

    // 第一步：先打开本地文件，拿到文件大小和后续读文件所需的 fd。
    int fd = open(arg, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        LOG_WARN("打开本地上传文件失败，文件=%s，错误码=%d", arg, errno);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("获取文件大小失败");
        LOG_ERROR("读取本地上传文件信息失败，文件=%s，错误码=%d", arg, errno);
        close(fd);
        return -1;
    }

    // 第一步补充：上传协议依赖文件 hash。
    // 服务端会用它来判断秒传、真实文件命名、以及断点续传归属。
    char file_hash[64] = {0};
    printf("正在计算文件哈希值...\n");
    if (get_file_sha256(arg, file_hash) == -1) {
        printf("计算文件哈希值失败\n");
        LOG_ERROR("计算文件哈希值失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    LOG_DEBUG("计算文件哈希值成功，文件=%s，哈希=%s", arg, file_hash);

    // 第二步：先发 puts 命令包，再发文件信息包。
    // 这两步完成后，服务端才能决定：秒传、续传还是从头上传。
    command_packet_t cmd_packet;
    init_command_packet(&cmd_packet, CMD_TYPE_PUTS, arg);
    if (send_command_packet(sock_fd, &cmd_packet) == -1) {
        printf("发送命令失败\n");
        LOG_ERROR("发送上传命令失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    file_packet_t client_file_packet;
    init_file_packet(&client_file_packet, CMD_TYPE_PUTS, arg, st.st_size, 0, file_hash);
    if (send_file_packet(sock_fd, &client_file_packet) == -1) {
        printf("发送文件信息失败\n");
        LOG_WARN("发送上传文件信息失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    // 第三步：接收服务端返回的断点信息。
    // 如果服务端已经有完整实体，会通过 hash 命中直接告诉客户端“秒传成功”。
    file_packet_t server_file_packet;
    if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
        printf("接收服务端断点信息失败\n");
        LOG_WARN("接收上传断点位置失败，文件=%s", arg);
        close(fd);
        return -1;
    }

    if (strcmp(server_file_packet.hash, file_hash) == 0) {
        printf("极速秒传成功。\n");
        LOG_INFO("上传已跳过，服务器文件完整，秒传完成，文件=%s，大小=%lld", arg, (long long)st.st_size);
        close(fd);
        return 0;
    }

    if (server_file_packet.offset > st.st_size) {
        server_file_packet.offset = 0;
    }

    LOG_DEBUG("上传断点信息，文件=%s，本地大小=%lld，偏移=%lld",
              arg,
              (long long)st.st_size,
              (long long)server_file_packet.offset);

    // 第四步：把本地文件读指针移动到服务端要求的断点位置。
    if (lseek(fd, server_file_packet.offset, SEEK_SET) == -1) {
        perror("移动文件指针失败");
        LOG_ERROR("定位本地上传文件失败，文件=%s，偏移=%lld，错误码=%d",
                  arg,
                  (long long)server_file_packet.offset,
                  errno);
        close(fd);
        return -1;
    }

    char buf[BUFFER_SIZE];

    // remaining 表示本次还需要继续向服务端发送多少字节。
    off_t remaining = st.st_size - server_file_packet.offset;

    // 第五步：循环读取本地文件并发给服务端，直到剩余数据全部发送完。
    while (remaining > 0) {
        int once = BUFFER_SIZE;
        if (remaining < BUFFER_SIZE) {
            once = (int)remaining;
        }

        int nread = read(fd, buf, once);
        if (nread <= 0) {
            break;
        }

        if (send_full(sock_fd, buf, nread) == -1) {
            printf("上传中断，已经发送的内容由服务端自己保留。\n");
            LOG_WARN("上传中断，文件=%s，已发送=%lld，总大小=%lld",
                     arg,
                     (long long)(st.st_size - remaining),
                     (long long)st.st_size);
            close(fd);
            return 0;
        }

        remaining -= nread;
    }

    // 第六步：本地文件内容发完后，再收一次服务端的最终文本结果。
    close(fd);
    recv_server_reply(sock_fd);
    LOG_INFO("上传成功，文件=%s", arg);
    return 0;
}
