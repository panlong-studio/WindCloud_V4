#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <openssl/rand.h>
#include "transfer_ticket.h"
#include "jwt_utils.h"
#include "log.h"

#define MAX_TRANSFER_TICKET_COUNT 1024
#define TRANSFER_TICKET_ID_LEN 33
#define TRANSFER_TICKET_EXPIRE_SECONDS 30

typedef struct {
    int active;                                      // 这一格当前是否正在使用
    char ticket_id[TRANSFER_TICKET_ID_LEN];          // 一次性票据唯一编号
    time_t expire_time;                              // 这张票据的过期时间
} TransferTicketNode;

static TransferTicketNode g_transfer_tickets[MAX_TRANSFER_TICKET_COUNT];
static pthread_mutex_t g_transfer_ticket_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief  把随机字节转换成十六进制字符串
 * @param  bin 输入二进制数据
 * @param  len 输入数据长度
 * @param  hex 输出参数，用来保存十六进制字符串
 * @return 无
 */
static void bin_to_hex(const unsigned char *bin, size_t len, char *hex) {
    size_t i = 0;

    for (i = 0; i < len; ++i) {
        sprintf(hex + i * 2, "%02x", bin[i]);
    }
    hex[len * 2] = '\0';
}

/**
 * @brief  生成一张传输票据唯一编号
 * @param  ticket_id 输出参数，用来保存生成后的编号
 * @param  size ticket_id 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int generate_ticket_id(char *ticket_id, size_t size) {
    unsigned char random_bytes[16];

    if (ticket_id == NULL || size < TRANSFER_TICKET_ID_LEN) {
        return -1;
    }

    // 16 字节随机数转成 32 位十六进制字符串，足够当前项目做一次性编号使用。
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
        return -1;
    }

    bin_to_hex(random_bytes, sizeof(random_bytes), ticket_id);
    return 0;
}

/**
 * @brief  清理票据表中已经过期的记录
 * @param  now 当前时间
 * @return 无
 */
static void cleanup_expired_tickets(time_t now) {
    int i = 0;

    for (i = 0; i < MAX_TRANSFER_TICKET_COUNT; ++i) {
        if (g_transfer_tickets[i].active == 1 &&
            g_transfer_tickets[i].expire_time <= now) {
            memset(&g_transfer_tickets[i], 0, sizeof(TransferTicketNode));
        }
    }
}

/**
 * @brief  在票据表里申请一个空闲槽位
 * @return 成功返回槽位下标，失败返回 -1
 */
static int alloc_ticket_slot(void) {
    int i = 0;

    for (i = 0; i < MAX_TRANSFER_TICKET_COUNT; ++i) {
        if (g_transfer_tickets[i].active == 0) {
            return i;
        }
    }

    return -1;
}

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
                          off_t range_start, off_t range_end,
                          const char *source_ip, const char *source_port,
                          char *ticket, size_t ticket_size) {
    char ticket_id[TRANSFER_TICKET_ID_LEN] = {0};
    time_t now = time(NULL);
    int slot = -1;

    if (full_path == NULL || full_path[0] == '\0' ||
        source_ip == NULL || source_ip[0] == '\0' ||
        source_port == NULL || source_port[0] == '\0' ||
        ticket == NULL || ticket_size == 0) {
        return -1;
    }

    // 第一步：先为这张票据生成一个唯一编号。
    if (generate_ticket_id(ticket_id, sizeof(ticket_id)) != 0) {
        return -1;
    }

    // 第二步：把用户、命令、路径、区间、数据源、票据编号一起写进 JWT 负载。
    if (jwt_create_transfer_ticket(user_id, cmd_type, full_path,
                                   range_start, range_end,
                                   source_ip, source_port,
                                   ticket_id, ticket, ticket_size) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_transfer_ticket_lock);

    // 每次签发新票据前，先把已过期记录清掉，避免表被历史垃圾占满。
    cleanup_expired_tickets(now);

    // 第三步：在内存票据表里登记这张票据，供“仅使用一次”的约束检查。
    slot = alloc_ticket_slot();
    if (slot == -1) {
        pthread_mutex_unlock(&g_transfer_ticket_lock);
        return -1;
    }

    g_transfer_tickets[slot].active = 1;
    snprintf(g_transfer_tickets[slot].ticket_id,
             sizeof(g_transfer_tickets[slot].ticket_id),
             "%s",
             ticket_id);
    g_transfer_tickets[slot].expire_time = now + TRANSFER_TICKET_EXPIRE_SECONDS;

    pthread_mutex_unlock(&g_transfer_ticket_lock);
    LOG_INFO("一次性传输票据签发成功，用户=%d，命令类型=%d，路径=%s，范围=%lld-%lld，数据源=%s:%s，票据编号=%s",
             user_id,
             cmd_type,
             full_path,
             (long long)range_start,
             (long long)range_end,
             source_ip,
             source_port,
             ticket_id);
    return 0;
}

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
                                       char *full_path, size_t full_path_size,
                                       off_t *range_start, off_t *range_end,
                                       char *source_ip, size_t source_ip_size,
                                       char *source_port, size_t source_port_size) {
    char ticket_id[TRANSFER_TICKET_ID_LEN] = {0};
    int parsed_cmd_type = 0;
    time_t now = time(NULL);
    int i = 0;
    int found = 0;

    if (ticket == NULL || ticket[0] == '\0' ||
        user_id == NULL || cmd_type == NULL ||
        full_path == NULL || full_path_size == 0 ||
        range_start == NULL || range_end == NULL ||
        source_ip == NULL || source_ip_size == 0 ||
        source_port == NULL || source_port_size == 0) {
        return -1;
    }

    // 第一步：先校验 JWT 本身有没有被篡改、有没有过期，并把负载字段解析出来。
    if (jwt_verify_transfer_ticket(ticket,
                                   user_id,
                                   &parsed_cmd_type,
                                   full_path,
                                   full_path_size,
                                   range_start,
                                   range_end,
                                   source_ip,
                                   source_ip_size,
                                   source_port,
                                   source_port_size,
                                   ticket_id,
                                   sizeof(ticket_id)) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_transfer_ticket_lock);

    // 先统一清理过期票据，再查找当前票据，避免把已经过期的旧票据误判为仍然有效。
    cleanup_expired_tickets(now);

    // 第二步：再去内存票据表里查这张票据是否还处于“未使用”状态。
    for (i = 0; i < MAX_TRANSFER_TICKET_COUNT; ++i) {
        if (g_transfer_tickets[i].active == 1 &&
            strcmp(g_transfer_tickets[i].ticket_id, ticket_id) == 0) {
            // 找到后立刻清空，表示这张票据从这一刻开始失效。
            memset(&g_transfer_tickets[i], 0, sizeof(TransferTicketNode));
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&g_transfer_ticket_lock);

    if (found == 0) {
        LOG_WARN("一次性传输票据消费失败：票据不存在、已过期或已被使用");
        return -1;
    }

    *cmd_type = (cmd_type_t)parsed_cmd_type;
    LOG_INFO("一次性传输票据消费成功，用户=%d，命令类型=%d，路径=%s，范围=%lld-%lld，数据源=%s:%s，票据编号=%s",
             *user_id,
             parsed_cmd_type,
             full_path,
             (long long)(*range_start),
             (long long)(*range_end),
             source_ip,
             source_port,
             ticket_id);
    return 0;
}
