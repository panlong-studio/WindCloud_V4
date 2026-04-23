#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <errno.h>
#include "client_socket.h"
#include "config.h"
#include "client_command_handle.h"   // 新增头文件
#include "log.h"

/**
 * @brief  显示客户端未登录菜单，并处理登录/注册流程
 * @param  sock_fd 客户端套接字
 * @return 成功进入已登录状态返回 0，失败返回 -1
 */
static int client_login_menu(int sock_fd){
    char input[512];
    printf("\n================================\n");
    printf("\n  欢迎使用 WindCloud 云盘系统！  \n");
    printf("\n================================\n");

    while(1){
        printf("\n[未登录] 请选择操作：\n");
        printf("-> login <用户名>/<密码>\n");
        printf("-> register <用户名>/<密码>\n");
        printf("-> quit\n");
        printf("请输入命令: ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            return -1;
        }

        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, "quit") == 0) {
            printf("再见！\n");
            LOG_INFO("客户端请求退出");
            exit(0);
        }

        // 未登录阶段只接受 login / register / quit。
        // 其余输入会被忽略，继续停留在当前菜单循环中。
        if(strncmp(input,"login ",6)==0||strncmp(input,"register ",9)==0){
            int ret=process_command(sock_fd,input);
            if(ret==1&&strncmp(input,"login ",6)==0){
                LOG_INFO("用户登录成功");
                printf("\n>>>>>登录成功<<<<<\n");
                return 0;
            }
            else if(ret==1 && strncmp(input,"register ",9)==0){
                LOG_INFO("用户注册成功");
                printf("\n>>>>>注册成功 请登录<<<<<\n");
                continue; // 留在这个死循环里，继续等用户敲 login
            }
            else{
                LOG_ERROR("登录/注册失败，输入=%s", input);
                continue;
            }
        }
    }
}

/**
 * @brief  从配置文件读取指定键，读不到时回退到默认值
 * @param  key 配置项名称
 * @param  value 输出参数，用来保存最终值
 * @param  value_sz value 缓冲区大小
 * @param  default_value 默认值
 * @return 无
 */
static void load_value_or_default(const char *key, char *value, size_t value_sz, const char *default_value) {
    char tmp[256] = {0};

    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    snprintf(value, value_sz, "%s", default_value);
}

/**
 * @brief  初始化日志，并兼容不同启动目录下的日志相对路径
 * @param  level_str 日志级别字符串
 * @param  log_file 原始日志文件路径
 * @return 无
 */
static void init_log_with_fallback(const char *level_str, const char *log_file) {
    const char *real_log_file = log_file;

    // 工程从不同目录启动时，../log 和 ./log 哪个可用并不固定。
    // 这里按实际存在的目录修正日志路径，避免因为路径问题导致日志初始化失败。
    if (log_file != NULL && strncmp(log_file, "../", 3) == 0) {
        if (access("../log", F_OK) == 0) {
            real_log_file = log_file;
        } else if (access("./log", F_OK) == 0) {
            real_log_file = log_file + 3;
        }
    }

    if (init_log(level_str, real_log_file) == 0) {
        return;
    }

    init_log(level_str, NULL);
}

/**
 * @brief  客户端主函数，负责初始化连接、登录菜单和命令循环
 * @param  argc 命令行参数个数
 * @param  argv 命令行参数数组
 * @return 程序正常结束返回 0，失败返回 -1
 */
int main(int argc, char *argv[])
{
    // 这两行只是为了消除“未使用参数”警告。
    (void)argc;
    (void)argv;

    // 准备两个字符数组，分别保存服务器 IP 和端口。
    char ip[64] = {0};
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};

    // 先用默认日志路径初始化，确保配置加载阶段的日志也能落盘。
    init_log_with_fallback("INFO", "../log/client.log");

    // 从配置文件中读取 IP、端口和日志参数。
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("client_log", log_file, sizeof(log_file), "../log/client.log");

    // 先初始化日志。
    // 否则后面如果 connect 失败，ERROR_CHECK 里打印日志时可能没有输出目标。
    init_log_with_fallback(log_level, log_file);
    signal(SIGPIPE, SIG_IGN);

    // sock_fd 就是客户端和服务端通信用的 socket。
    int sock_fd = 0;

    // 主动连接到服务端。
    init_socket(&sock_fd, ip, port);
    LOG_INFO("客户端已连接服务器，地址=%s，端口=%s", ip, port);

    // 进入登录/注册菜单。
    if(client_login_menu(sock_fd) == -1) {
        LOG_ERROR("客户端登录/注册菜单发生错误");
        close(sock_fd);
        return -1;
    }

    // input 用来保存用户每次输入的一整行命令。
    char input[512];

    // 客户端进入命令循环。
    // 从这里开始，所有已登录命令都会统一交给 process_command 处理。
    while (1) {
        // 打印命令提示符。
        printf("> ");

        // 立刻把提示符刷到终端上，避免缓冲区里还没显示。
        fflush(stdout);

        // fgets 从标准输入读一整行。
        // 如果返回 NULL，通常表示输入结束，例如按下 Ctrl+D。
        if (fgets(input, sizeof(input), stdin) == NULL) {
            LOG_INFO("客户端输入结束");
            break;
        }

        // fgets 通常会把末尾的 '\n' 一起读进来。
        // 这里把它替换成 '\0'，让字符串更方便后续处理。
        input[strcspn(input, "\n")] = '\0';

        // 用户输入 quit 或 exit 时，客户端主动退出。
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("再见！\n");
            LOG_INFO("客户端请求退出");
            break;
        }

        // 如果用户只输入了一个空行，就继续下一轮循环。
        if (strlen(input) == 0) {
            continue;
        }

        // 真正的命令发送、上传下载、结果接收，都交给 process_command 去做。
        LOG_DEBUG("客户端开始处理命令，输入=%s", input);
        process_command(sock_fd, input);
    }

    // 退出前关闭 socket。
    close(sock_fd);
    LOG_INFO("客户端套接字已关闭");

    // 关闭日志系统。
    close_log();
    return 0;
}
