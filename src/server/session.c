#include <stdio.h>
#include <string.h>
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "file_cmds.h"
#include "file_transfer.h"
#include "auth.h"

/**
 * @brief  向客户端发送一条普通文本响应
 * @param  client_fd 当前客户端套接字
 * @param  msg 要发送的文本消息
 * @return 无
 */
void send_msg(int client_fd, const char *msg) {
    command_packet_t reply_packet;

    // 服务端的普通响应统一封装成 CMD_TYPE_REPLY。
    // 客户端收到后会按普通文本路径直接打印。
    init_command_packet(&reply_packet, CMD_TYPE_REPLY, msg);

    if (send_command_packet(client_fd, &reply_packet) == -1) {
        LOG_WARN("发送响应失败，客户端fd=%d，消息=%s", client_fd, msg);
        return;
    }
    LOG_DEBUG("响应发送成功，客户端fd=%d，消息=%s", client_fd, msg);
}

/**
 * @brief  从普通命令包中解析命令类型
 * @param  cmd_packet 已接收的普通命令包
 * @return 成功返回命令类型，失败返回 CMD_TYPE_INVALID
 */
static cmd_type_t get_packet_cmd_type(const command_packet_t *cmd_packet) {
    if (cmd_packet == NULL) {
        return CMD_TYPE_INVALID;
    }
    return (cmd_type_t)cmd_packet->cmd_type;
}

/**
 * @brief  处理一个客户端连接上的完整请求生命周期
 * @param  client_fd 当前客户端套接字
 * @return 无
 */
void handle_request(int client_fd) {
    ClientContext ctx;

    // 每个客户端连接，都维护一个独立的 ClientContext。
    // 它就像“这个用户当前会话的小档案”，里面保存：
    // 1. 当前是谁（user_id）
    // 2. 当前在哪个虚拟目录（current_path）
    // 3. 当前目录节点 id 是多少（current_dir_id）
    // 后面 pwd / cd / ls / puts / gets 全都依赖它。
    //
    // 先把整个上下文清零，避免里面残留脏数据。
    memset(&ctx, 0, sizeof(ctx));

    // 新连接默认还没有登录。
    ctx.user_id = -1;

    // 初始虚拟路径固定为根目录。
    strcpy(ctx.current_path, "/");

    // 根目录约定 current_dir_id 为 0。
    ctx.current_dir_id = 0;

    while (1) {
        command_packet_t cmd_packet;

        // handle_request 是“一个客户端连接上的总循环”。
        // 客户端不断发命令，这里就不断收命令。
        // 一旦 recv_command_packet 返回 <= 0，说明连接断开或出错，这个会话就结束。
        if (recv_command_packet(client_fd, &cmd_packet) <= 0) {
            LOG_INFO("客户端连接断开，客户端fd=%d，当前路径=%s", client_fd, ctx.current_path);
            break;
        }

        cmd_type_t cmd_type = get_packet_cmd_type(&cmd_packet);
        LOG_DEBUG("收到客户端命令，客户端fd=%d，命令类型=%d，数据=%s", client_fd, cmd_type, cmd_packet.data);

        // 没登录时，除了登录和注册，别的命令都不允许执行。
        if(ctx.user_id == -1 && cmd_type != CMD_TYPE_LOGIN && cmd_type != CMD_TYPE_REGISTER) {
            LOG_WARN("未登录用户尝试执行命令，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
            send_msg(client_fd, "请先登录!");
            continue;
        }

        // 进入统一命令分发。
        // 每条命令都会基于当前会话上下文 ctx 继续处理。
        switch (cmd_type) {
            case CMD_TYPE_LOGIN: {
                int old_user_id = ctx.user_id;// 记录登录前的 user_id，看看登录后有没有变化
                handle_login(client_fd, cmd_packet.data, &ctx.user_id);

                // 登录成功后，把会话路径重新放回根目录。
                // 这样无论这个连接之前是什么状态，一旦登录成功，
                // 后续目录类命令都从 "/" 开始，逻辑最清楚。
                if (old_user_id == -1 && ctx.user_id != -1) {// 之前没登录，现在登录成功了
                    LOG_INFO("用户登录成功，客户端fd=%d，用户id=%d", client_fd, ctx.user_id);
                    strcpy(ctx.current_path, "/");
                    ctx.current_dir_id = 0;
                }
                break;
            }

            case CMD_TYPE_REGISTER: {
                int temp_new_id=-1;
                handle_register(client_fd, cmd_packet.data, &temp_new_id);

                // 当前注册成功后，客户端仍然需要再执行一次 login。
                if(temp_new_id!=-1){
                    LOG_INFO("新用户注册成功，客户端fd=%d，新用户id=%d", client_fd, temp_new_id);
                } else {
                    LOG_WARN("用户注册失败，客户端fd=%d", client_fd);
                }
                break;
            }

            case CMD_TYPE_PWD:
                handle_pwd(client_fd, &ctx);
                break;
            case CMD_TYPE_CD:
                handle_cd(client_fd, &ctx, cmd_packet.data);
                break;
            case CMD_TYPE_LS:
                handle_ls(client_fd, &ctx);
                break;
            case CMD_TYPE_GETS:
                handle_gets(client_fd, &ctx, cmd_packet.data);
                break;
            case CMD_TYPE_PUTS:
                handle_puts(client_fd, &ctx, cmd_packet.data);
                break;
            case CMD_TYPE_TOUCH:
                handle_touch(client_fd, &ctx, cmd_packet.data);
                break;
            case CMD_TYPE_RM:
                handle_rm(client_fd, &ctx, cmd_packet.data);
                break;
            case CMD_TYPE_MKDIR:
                handle_mkdir(client_fd, &ctx, cmd_packet.data);
                break;
            case CMD_TYPE_RMDIR:
                handle_rmdir(client_fd, &ctx, cmd_packet.data);
                break;
            default:
                LOG_WARN("命令类型无效，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
                send_msg(client_fd, "指令错误!");
                break;
        }
    }
}
