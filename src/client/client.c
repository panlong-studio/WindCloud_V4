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
 * @param  app_ctx 客户端上下文
 * @return 成功进入已登录状态返回 0，失败返回 -1
 */
static int client_login_menu(ClientAppContext *app_ctx){
    char input[512];
    printf("\n================================\n");
    printf("\n  欢迎使用 WindCloud 云盘系统！  \n");
    printf("\n================================\n");

    /* 持续停留在未登录菜单，直到登录成功或用户主动退出。 */
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

        /* 未登录阶段只接受 login、register 和 quit。 */
        if(strncmp(input,"login ",6)==0||strncmp(input,"register ",9)==0){
            int ret=process_command(app_ctx,input);
            if(ret==1&&strncmp(input,"login ",6)==0){
                LOG_INFO("用户登录成功");
                printf("\n>>>>>登录成功<<<<<\n");
                return 0;
            }
            else if(ret==1 && strncmp(input,"register ",9)==0){
                LOG_INFO("用户注册成功");
                printf("\n>>>>>注册成功 请登录<<<<<\n");
                continue;
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

    /* 优先读取配置文件中的值。 */
    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    /* 配置缺失时，使用调用方提供的默认值。 */
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

    /* 第一步：根据当前启动目录修正日志路径。 */
    if (log_file != NULL && strncmp(log_file, "../", 3) == 0) {
        if (access("../log", F_OK) == 0) {
            real_log_file = log_file;
        } else if (access("./log", F_OK) == 0) {
            real_log_file = log_file + 3;
        }
    }

    /* 第二步：优先按修正后的路径初始化日志。 */
    if (init_log(level_str, real_log_file) == 0) {
        return;
    }

    /* 第三步：如果文件日志初始化失败，则退回标准输出。 */
    init_log(level_str, NULL);
}

/**
 * @brief  在主连接失效后，重新建立主连接并重新进入登录菜单
 * @param  app_ctx 客户端上下文
 * @return 成功返回 0，失败返回 -1
 */
static int reconnect_and_login(ClientAppContext *app_ctx) {
    if (app_ctx == NULL) {
        return -1;
    }

    /* 第一步：关闭旧主连接，避免继续使用失效套接字。 */
    close(app_ctx->sock_fd);

    /* 第二步：重新建立主连接。 */
    init_socket(&app_ctx->sock_fd, app_ctx->server_ip, app_ctx->server_port);

    /* 第三步：回到登录菜单，重新获取有效 token。 */
    if (client_login_menu(app_ctx) == -1) {
        return -1;
    }

    return 0;
}

/**
 * @brief  客户端主函数，负责初始化连接、登录菜单和命令循环
 * @param  argc 命令行参数个数
 * @param  argv 命令行参数数组
 * @return 程序正常结束返回 0，失败返回 -1
 */
int main(int argc, char *argv[])
{
    /* 这两行用于消除“未使用参数”警告。 */
    (void)argc;
    (void)argv;

    /* 准备配置项缓冲区。 */
    char ip[64] = {0};
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};

    /* 第一步：先用默认日志路径初始化日志，保证配置读取阶段也能输出日志。 */
    init_log_with_fallback("INFO", "../log/client.log");

    /* 第二步：读取 IP、端口和日志配置。 */
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("client_log", log_file, sizeof(log_file), "../log/client.log");

    /* 第三步：按正式配置重新初始化日志。 */
    init_log_with_fallback(log_level, log_file);
    signal(SIGPIPE, SIG_IGN);

    /* sock_fd 是客户端主连接套接字。 */
    int sock_fd = 0;

    /* 第四步：主动连接服务端。 */
    init_socket(&sock_fd, ip, port);
    LOG_INFO("客户端已连接服务器，地址=%s，端口=%s", ip, port);

    ClientAppContext app_ctx;
    client_app_context_init(&app_ctx, sock_fd, ip, port);

    /* 第五步：进入登录菜单，获取登录态和 token。 */
    if(client_login_menu(&app_ctx) == -1) {
        LOG_ERROR("客户端登录/注册菜单发生错误");
        close(sock_fd);
        return -1;
    }

    /* input 用来保存用户输入的一整行命令。 */
    char input[512];

    /* 第六步：进入已登录命令循环。 */
    while (1) {
        printf("> ");
        fflush(stdout);

        /* 从标准输入读取一整行命令。 */
        if (fgets(input, sizeof(input), stdin) == NULL) {
            LOG_INFO("客户端输入结束");
            break;
        }

        /* 去掉行尾换行符，便于后续解析。 */
        input[strcspn(input, "\n")] = '\0';

        /* 用户输入 quit 或 exit 时，客户端主动退出。 */
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("再见！\n");
            LOG_INFO("客户端请求退出");
            break;
        }

        /* 空行直接忽略。 */
        if (strlen(input) == 0) {
            continue;
        }

        /* 第七步：把命令统一交给命令分发模块处理。 */
        LOG_DEBUG("客户端开始处理命令，输入=%s", input);
        int ret = process_command(&app_ctx, input);
        if (ret == -1 && app_ctx.is_logged_in == 0) {
            printf("主连接已失效，请重新登录。\n");
            LOG_WARN("检测到主连接失效，准备重新登录");

            /* 主连接失效后，重新连接并回到登录菜单。 */
            if (reconnect_and_login(&app_ctx) == -1) {
                LOG_ERROR("客户端重新连接或重新登录失败");
                break;
            }
        }
    }

    /* 第八步：退出前关闭主连接并清理日志。 */
    close(sock_fd);
    LOG_INFO("客户端套接字已关闭");

    close_log();
    return 0;
}
