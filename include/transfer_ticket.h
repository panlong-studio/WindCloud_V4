#ifndef TRANSFER_TICKET_H
#define TRANSFER_TICKET_H

#include <stddef.h>
#include "protocol.h"

/**
 * @brief  为本次上传或下载申请一张短时一次性传输票据
 * @param  user_id 用户 id
 * @param  cmd_type 传输命令类型
 * @param  full_path 目标完整虚拟路径
 * @param  ticket 输出参数，用来保存生成后的票据
 * @param  ticket_size ticket 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int issue_transfer_ticket(int user_id, cmd_type_t cmd_type, const char *full_path,
                          char *ticket, size_t ticket_size);

/**
 * @brief  校验并消费一张一次性传输票据
 * @param  ticket 传输票据字符串
 * @param  user_id 输出参数，用来保存用户 id
 * @param  cmd_type 输出参数，用来保存命令类型
 * @param  full_path 输出参数，用来保存完整虚拟路径
 * @param  full_path_size full_path 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int verify_and_consume_transfer_ticket(const char *ticket, int *user_id, cmd_type_t *cmd_type,
                                       char *full_path, size_t full_path_size);

#endif
