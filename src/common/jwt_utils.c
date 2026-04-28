#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <l8w8jwt/encode.h>
#include <l8w8jwt/decode.h>
#include "jwt_utils.h"
#include "protocol.h"

#define JWT_SECRET_KEY "windcloud jwt secret key"
#define LOGIN_JWT_ISSUER "WindCloud"
#define LOGIN_JWT_AUDIENCE "WindCloudClient"
#define LOGIN_JWT_EXPIRE_SECONDS 3600
#define TRANSFER_TICKET_ISSUER "WindCloudTransfer"
#define TRANSFER_TICKET_AUDIENCE "WindCloudTransferClient"
#define TRANSFER_TICKET_EXPIRE_SECONDS 30

/**
 * @brief  从 claim 数组中提取字符串字段，并复制到目标缓冲区
 * @param  claims claim 数组
 * @param  claim_count claim 数组长度
 * @param  key 要查找的字段名
 * @param  key_length 字段名长度
 * @param  output 输出缓冲区
 * @param  output_size 输出缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int copy_string_claim(struct l8w8jwt_claim *claims, size_t claim_count,
                             const char *key, size_t key_length,
                             char *output, size_t output_size) {
    struct l8w8jwt_claim *claim = NULL;

    if (claims == NULL || key == NULL || output == NULL || output_size == 0) {
        return -1;
    }

    claim = l8w8jwt_get_claim(claims, claim_count, key, key_length);
    if (claim == NULL || claim->value == NULL) {
        return -1;
    }

    if (strlen(claim->value) + 1 > output_size) {
        return -1;
    }

    snprintf(output, output_size, "%s", claim->value);
    return 0;
}

/**
 * @brief  为指定用户生成 JWT 字符串
 * @param  user_id 用户 id
 * @param  token 输出参数，用来保存生成后的 JWT
 * @param  token_size token 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_create_for_user(int user_id, char *token, size_t token_size) {
    char uid_buf[32] = {0};
    char *jwt_str = NULL;
    size_t jwt_length = 0;
    struct l8w8jwt_encoding_params params;
    struct l8w8jwt_claim extra_claim;
    time_t now = time(NULL);

    if (token == NULL || token_size == 0) {
        return -1;
    }

    snprintf(uid_buf, sizeof(uid_buf), "%d", user_id);

    // 登录 JWT 当前只放一个最核心的 uid 字段。
    // 目录状态仍然继续保留在控制连接自己的 ClientContext 里维护。
    memset(&extra_claim, 0, sizeof(extra_claim));
    extra_claim.key = "uid";
    extra_claim.key_length = 3;
    extra_claim.value = uid_buf;
    extra_claim.value_length = strlen(uid_buf);
    extra_claim.type = L8W8JWT_CLAIM_TYPE_INTEGER;

    // 这里统一设置发行者、接收方、签发时间、过期时间和密钥。
    l8w8jwt_encoding_params_init(&params);
    params.alg = L8W8JWT_ALG_HS256;
    params.iss = LOGIN_JWT_ISSUER;
    params.iss_length = strlen(LOGIN_JWT_ISSUER);
    params.aud = LOGIN_JWT_AUDIENCE;
    params.aud_length = strlen(LOGIN_JWT_AUDIENCE);
    params.iat = (l8w8jwt_time_t)now;
    params.exp = (l8w8jwt_time_t)(now + LOGIN_JWT_EXPIRE_SECONDS);
    params.additional_payload_claims = &extra_claim;
    params.additional_payload_claims_count = 1;
    params.secret_key = (unsigned char *)JWT_SECRET_KEY;
    params.secret_key_length = strlen(JWT_SECRET_KEY);
    params.out = &jwt_str;
    params.out_length = &jwt_length;

    if (l8w8jwt_encode(&params) != L8W8JWT_SUCCESS) {
        return -1;
    }

    if (jwt_length + 1 > token_size) {
        l8w8jwt_free(jwt_str);
        return -1;
    }

    strncpy(token, jwt_str, token_size - 1);
    token[token_size - 1] = '\0';
    l8w8jwt_free(jwt_str);
    return 0;
}

/**
 * @brief  校验 JWT 并解析出用户 id
 * @param  token JWT 字符串
 * @param  user_id 输出参数，用来保存解析出的用户 id
 * @return 成功返回 0，失败返回 -1
 */
int jwt_verify_and_get_user(const char *token, int *user_id) {
    struct l8w8jwt_decoding_params params;
    struct l8w8jwt_claim *claims = NULL;
    struct l8w8jwt_claim *uid_claim = NULL;
    size_t claim_count = 0;
    enum l8w8jwt_validation_result validation_result;
    int decode_result = 0;

    if (token == NULL || user_id == NULL || token[0] == '\0') {
        return -1;
    }

    l8w8jwt_decoding_params_init(&params);
    params.alg = L8W8JWT_ALG_HS256;
    params.jwt = (char *)token;
    params.jwt_length = strlen(token);
    params.validate_iss = LOGIN_JWT_ISSUER;
    params.validate_iss_length = strlen(LOGIN_JWT_ISSUER);
    params.validate_aud = LOGIN_JWT_AUDIENCE;
    params.validate_aud_length = strlen(LOGIN_JWT_AUDIENCE);
    params.validate_exp = 1;
    params.verification_key = (unsigned char *)JWT_SECRET_KEY;
    params.verification_key_length = strlen(JWT_SECRET_KEY);

    // 先做完整的 JWT 校验：
    // 1. 签名正确
    // 2. issuer 正确
    // 3. audience 正确
    // 4. exp 没过期
    decode_result = l8w8jwt_decode(&params, &validation_result, &claims, &claim_count);
    if (decode_result != L8W8JWT_SUCCESS || validation_result != L8W8JWT_VALID) {
        if (claims != NULL) {
            l8w8jwt_free_claims(claims, claim_count);
        }
        return -1;
    }

    // 校验通过后，再从负载里把 uid 字段取出来。
    uid_claim = l8w8jwt_get_claim(claims, claim_count, "uid", 3);
    if (uid_claim == NULL || uid_claim->value == NULL) {
        l8w8jwt_free_claims(claims, claim_count);
        return -1;
    }

    *user_id = atoi(uid_claim->value);
    l8w8jwt_free_claims(claims, claim_count);
    return 0;
}

/**
 * @brief  为指定用户和目标文件生成短时一次性传输票据
 * @param  user_id 用户 id
 * @param  cmd_type 传输命令类型
 * @param  full_path 完整虚拟路径
 * @param  ticket_id 票据唯一编号
 * @param  ticket 输出参数，用来保存生成后的传输票据
 * @param  ticket_size ticket 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_create_transfer_ticket(int user_id, int cmd_type, const char *full_path,
                               off_t range_start, off_t range_end,
                               const char *source_ip, const char *source_port,
                               const char *ticket_id, char *ticket, size_t ticket_size) {
    char uid_buf[32] = {0};
    char cmd_buf[32] = {0};
    char start_buf[32] = {0};
    char end_buf[32] = {0};
    char *jwt_str = NULL;
    size_t jwt_length = 0;
    struct l8w8jwt_encoding_params params;
    struct l8w8jwt_claim extra_claims[8];
    time_t now = time(NULL);

    if (full_path == NULL || full_path[0] == '\0' ||
        source_ip == NULL || source_ip[0] == '\0' ||
        source_port == NULL || source_port[0] == '\0' ||
        ticket_id == NULL || ticket_id[0] == '\0' ||
        ticket == NULL || ticket_size == 0) {
        return -1;
    }

    // 先把所有数值字段转成字符串。
    // l8w8jwt 的 claim 接口最终接收的是字符串指针和长度。
    snprintf(uid_buf, sizeof(uid_buf), "%d", user_id);
    snprintf(cmd_buf, sizeof(cmd_buf), "%d", cmd_type);
    snprintf(start_buf, sizeof(start_buf), "%lld", (long long)range_start);
    snprintf(end_buf, sizeof(end_buf), "%lld", (long long)range_end);
    memset(extra_claims, 0, sizeof(extra_claims));

    // 票据里一共放 8 个字段：
    // uid/cmd/path/tid/start/end/sip/sport
    // 这样传输连接在不依赖控制连接内存状态的前提下，也能独立完成权限校验。
    extra_claims[0].key = "uid";
    extra_claims[0].key_length = 3;
    extra_claims[0].value = uid_buf;
    extra_claims[0].value_length = strlen(uid_buf);
    extra_claims[0].type = L8W8JWT_CLAIM_TYPE_INTEGER;

    extra_claims[1].key = "cmd";
    extra_claims[1].key_length = 3;
    extra_claims[1].value = cmd_buf;
    extra_claims[1].value_length = strlen(cmd_buf);
    extra_claims[1].type = L8W8JWT_CLAIM_TYPE_INTEGER;

    extra_claims[2].key = "path";
    extra_claims[2].key_length = 4;
    extra_claims[2].value = (char *)full_path;
    extra_claims[2].value_length = strlen(full_path);
    extra_claims[2].type = L8W8JWT_CLAIM_TYPE_STRING;

    extra_claims[3].key = "tid";
    extra_claims[3].key_length = 3;
    extra_claims[3].value = (char *)ticket_id;
    extra_claims[3].value_length = strlen(ticket_id);
    extra_claims[3].type = L8W8JWT_CLAIM_TYPE_STRING;

    extra_claims[4].key = "start";
    extra_claims[4].key_length = 5;
    extra_claims[4].value = start_buf;
    extra_claims[4].value_length = strlen(start_buf);
    extra_claims[4].type = L8W8JWT_CLAIM_TYPE_INTEGER;

    extra_claims[5].key = "end";
    extra_claims[5].key_length = 3;
    extra_claims[5].value = end_buf;
    extra_claims[5].value_length = strlen(end_buf);
    extra_claims[5].type = L8W8JWT_CLAIM_TYPE_INTEGER;

    extra_claims[6].key = "sip";
    extra_claims[6].key_length = 3;
    extra_claims[6].value = (char *)source_ip;
    extra_claims[6].value_length = strlen(source_ip);
    extra_claims[6].type = L8W8JWT_CLAIM_TYPE_STRING;

    extra_claims[7].key = "sport";
    extra_claims[7].key_length = 5;
    extra_claims[7].value = (char *)source_port;
    extra_claims[7].value_length = strlen(source_port);
    extra_claims[7].type = L8W8JWT_CLAIM_TYPE_STRING;

    // 传输票据和登录 JWT 使用不同的 issuer/audience，
    // 这样两类令牌的用途是分开的，不能混用。
    l8w8jwt_encoding_params_init(&params);
    params.alg = L8W8JWT_ALG_HS256;
    params.iss = TRANSFER_TICKET_ISSUER;
    params.iss_length = strlen(TRANSFER_TICKET_ISSUER);
    params.aud = TRANSFER_TICKET_AUDIENCE;
    params.aud_length = strlen(TRANSFER_TICKET_AUDIENCE);
    params.iat = (l8w8jwt_time_t)now;
    params.exp = (l8w8jwt_time_t)(now + TRANSFER_TICKET_EXPIRE_SECONDS);
    params.additional_payload_claims = extra_claims;
    params.additional_payload_claims_count = 8;
    params.secret_key = (unsigned char *)JWT_SECRET_KEY;
    params.secret_key_length = strlen(JWT_SECRET_KEY);
    params.out = &jwt_str;
    params.out_length = &jwt_length;

    if (l8w8jwt_encode(&params) != L8W8JWT_SUCCESS) {
        return -1;
    }

    if (jwt_length + 1 > ticket_size) {
        l8w8jwt_free(jwt_str);
        return -1;
    }

    strncpy(ticket, jwt_str, ticket_size - 1);
    ticket[ticket_size - 1] = '\0';
    l8w8jwt_free(jwt_str);
    return 0;
}

/**
 * @brief  校验一次性传输票据并解析出其负载信息
 * @param  ticket 传输票据字符串
 * @param  user_id 输出参数，用来保存用户 id
 * @param  cmd_type 输出参数，用来保存命令类型
 * @param  full_path 输出参数，用来保存完整虚拟路径
 * @param  full_path_size full_path 缓冲区大小
 * @param  ticket_id 输出参数，用来保存票据唯一编号
 * @param  ticket_id_size ticket_id 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_verify_transfer_ticket(const char *ticket, int *user_id, int *cmd_type,
                               char *full_path, size_t full_path_size,
                               off_t *range_start, off_t *range_end,
                               char *source_ip, size_t source_ip_size,
                               char *source_port, size_t source_port_size,
                               char *ticket_id, size_t ticket_id_size) {
    struct l8w8jwt_decoding_params params;
    struct l8w8jwt_claim *claims = NULL;
    struct l8w8jwt_claim *uid_claim = NULL;
    struct l8w8jwt_claim *cmd_claim = NULL;
    struct l8w8jwt_claim *start_claim = NULL;
    struct l8w8jwt_claim *end_claim = NULL;
    size_t claim_count = 0;
    enum l8w8jwt_validation_result validation_result;
    int decode_result = 0;

    if (ticket == NULL || ticket[0] == '\0' ||
        user_id == NULL || cmd_type == NULL ||
        full_path == NULL || full_path_size == 0 ||
        range_start == NULL || range_end == NULL ||
        source_ip == NULL || source_ip_size == 0 ||
        source_port == NULL || source_port_size == 0 ||
        ticket_id == NULL || ticket_id_size == 0) {
        return -1;
    }

    l8w8jwt_decoding_params_init(&params);
    params.alg = L8W8JWT_ALG_HS256;
    params.jwt = (char *)ticket;
    params.jwt_length = strlen(ticket);
    params.validate_iss = TRANSFER_TICKET_ISSUER;
    params.validate_iss_length = strlen(TRANSFER_TICKET_ISSUER);
    params.validate_aud = TRANSFER_TICKET_AUDIENCE;
    params.validate_aud_length = strlen(TRANSFER_TICKET_AUDIENCE);
    params.validate_exp = 1;
    params.verification_key = (unsigned char *)JWT_SECRET_KEY;
    params.verification_key_length = strlen(JWT_SECRET_KEY);

    // 第一步：先确认这是一张合法的“传输票据”而不是别的 JWT。
    decode_result = l8w8jwt_decode(&params, &validation_result, &claims, &claim_count);
    if (decode_result != L8W8JWT_SUCCESS || validation_result != L8W8JWT_VALID) {
        if (claims != NULL) {
            l8w8jwt_free_claims(claims, claim_count);
        }
        return -1;
    }

    // 第二步：把关键整数字段先取出来。
    uid_claim = l8w8jwt_get_claim(claims, claim_count, "uid", 3);
    cmd_claim = l8w8jwt_get_claim(claims, claim_count, "cmd", 3);
    start_claim = l8w8jwt_get_claim(claims, claim_count, "start", 5);
    end_claim = l8w8jwt_get_claim(claims, claim_count, "end", 3);
    if (uid_claim == NULL || uid_claim->value == NULL ||
        cmd_claim == NULL || cmd_claim->value == NULL ||
        start_claim == NULL || start_claim->value == NULL ||
        end_claim == NULL || end_claim->value == NULL) {
        l8w8jwt_free_claims(claims, claim_count);
        return -1;
    }

    // 第三步：把路径、数据源、票据编号等字符串字段复制出来。
    if (copy_string_claim(claims, claim_count, "path", 4, full_path, full_path_size) != 0 ||
        copy_string_claim(claims, claim_count, "sip", 3, source_ip, source_ip_size) != 0 ||
        copy_string_claim(claims, claim_count, "sport", 5, source_port, source_port_size) != 0 ||
        copy_string_claim(claims, claim_count, "tid", 3, ticket_id, ticket_id_size) != 0) {
        l8w8jwt_free_claims(claims, claim_count);
        return -1;
    }

    // 第四步：把字符串数字再转回本项目内部使用的整数类型。
    *user_id = atoi(uid_claim->value);
    *cmd_type = atoi(cmd_claim->value);
    *range_start = (off_t)atoll(start_claim->value);
    *range_end = (off_t)atoll(end_claim->value);
    l8w8jwt_free_claims(claims, claim_count);
    return 0;
}
