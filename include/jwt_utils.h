#ifndef JWT_UTILS_H
#define JWT_UTILS_H

#include <stddef.h>
#include <sys/types.h>

/**
 * @brief  为指定用户生成 JWT 字符串
 * @param  user_id 用户 id
 * @param  token 输出参数，用来保存生成后的 JWT
 * @param  token_size token 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_create_for_user(int user_id, char *token, size_t token_size);

/**
 * @brief  校验 JWT 并解析出用户 id
 * @param  token JWT 字符串
 * @param  user_id 输出参数，用来保存解析出的用户 id
 * @return 成功返回 0，失败返回 -1
 */
int jwt_verify_and_get_user(const char *token, int *user_id);

/**
 * @brief  为指定用户和目标文件生成短时一次性传输票据
 * @param  user_id 用户 id
 * @param  cmd_type 传输命令类型，必须是 CMD_TYPE_PUTS 或 CMD_TYPE_GETS
 * @param  full_path 目标完整虚拟路径
 * @param  range_start 本次允许下载或传输的起始位置
 * @param  range_end 本次允许下载或传输的结束位置
 * @param  source_ip 本次允许连接的数据源服务器 IP
 * @param  source_port 本次允许连接的数据源服务器端口
 * @param  ticket_id 票据唯一编号
 * @param  ticket 输出参数，用来保存生成后的传输票据
 * @param  ticket_size ticket 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_create_transfer_ticket(int user_id, int cmd_type, const char *full_path,
                               off_t range_start, off_t range_end,
                               const char *source_ip, const char *source_port,
                               const char *ticket_id, char *ticket, size_t ticket_size);

/**
 * @brief  校验一次性传输票据并解析出其负载信息
 * @param  ticket 传输票据字符串
 * @param  user_id 输出参数，用来保存用户 id
 * @param  cmd_type 输出参数，用来保存命令类型
 * @param  full_path 输出参数，用来保存完整虚拟路径
 * @param  full_path_size full_path 缓冲区大小
 * @param  range_start 输出参数，用来保存允许下载的起始位置
 * @param  range_end 输出参数，用来保存允许下载的结束位置
 * @param  source_ip 输出参数，用来保存票据绑定的数据源服务器 IP
 * @param  source_ip_size source_ip 缓冲区大小
 * @param  source_port 输出参数，用来保存票据绑定的数据源服务器端口
 * @param  source_port_size source_port 缓冲区大小
 * @param  ticket_id 输出参数，用来保存票据唯一编号
 * @param  ticket_id_size ticket_id 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_verify_transfer_ticket(const char *ticket, int *user_id, int *cmd_type,
                               char *full_path, size_t full_path_size,
                               off_t *range_start, off_t *range_end,
                               char *source_ip, size_t source_ip_size,
                               char *source_port, size_t source_port_size,
                               char *ticket_id, size_t ticket_id_size);

#endif
