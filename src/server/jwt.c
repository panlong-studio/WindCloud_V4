#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "jwt.h"

#define JWT_HEADER_JSON "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"
#define JWT_DEFAULT_SECRET "WindCloud_V4_Default_JWT_Secret"
#define JWT_SIGN_LEN 32

static char g_jwt_secret[JWT_SECRET_LEN] = JWT_DEFAULT_SECRET;

/**
 * @brief  把普通 Base64 字符串改成 URL 安全格式
 * @param  str 要修改的字符串
 * @return 无
 */
static void base64_to_base64url(char *str) {
    int idx = 0;
    int write_idx = 0;

    // 逐字符扫描普通 Base64 字符串，并改写为 Base64URL 格式。
    // 处理规则如下：
    // 1. '+' 替换为 '-'
    // 2. '/' 替换为 '_'
    // 3. 结尾的 '=' 补位字符删除
    while (str[idx] != '\0') {
        if (str[idx] == '+') {
            str[write_idx++] = '-';
        } else if (str[idx] == '/') {
            str[write_idx++] = '_';
        } else if (str[idx] == '=') {
            idx++;
            continue;
        } else {
            str[write_idx++] = str[idx];
        }
        idx++;
    }

    str[write_idx] = '\0';
}

/**
 * @brief  把 Base64URL 字符串恢复成普通 Base64 格式
 * @param  src 输入的 Base64URL 字符串
 * @param  dst 输出参数，用来保存恢复后的 Base64 字符串
 * @param  dst_size dst 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int base64url_to_base64(const char *src, char *dst, size_t dst_size) {
    size_t src_len = strlen(src);
    size_t need_len = src_len;

    // 第一步：把目标长度补齐到 4 的倍数。
    // 普通 Base64 需要按 4 字节对齐，因此需要补回 '='。
    while (need_len % 4 != 0) {
        need_len++;
    }

    // 第二步：确认目标缓冲区大小足够。
    if (need_len + 1 > dst_size) {
        return -1;
    }

    // 第三步：逐字符恢复普通 Base64 字符。
    size_t idx = 0;
    for (; idx < src_len; ++idx) {
        if (src[idx] == '-') {
            dst[idx] = '+';
        } else if (src[idx] == '_') {
            dst[idx] = '/';
        } else {
            dst[idx] = src[idx];
        }
    }

    // 第四步：补齐 '='。
    while (idx < need_len) {
        dst[idx++] = '=';
    }

    // 第五步：补字符串结束符。
    dst[need_len] = '\0';
    return 0;
}

/**
 * @brief  把二进制数据做 Base64URL 编码
 * @param  src 输入二进制数据
 * @param  src_len 输入数据长度
 * @param  dst 输出参数，用来保存编码后的字符串
 * @param  dst_size dst 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int base64url_encode(const unsigned char *src, size_t src_len, char *dst, size_t dst_size) {
    int base64_len = 4 * ((int)(src_len + 2) / 3);
    char *tmp = NULL;

    // 第一步：预估普通 Base64 编码后的长度，并检查输出缓冲区大小。
    if ((size_t)base64_len + 1 > dst_size) {
        return -1;
    }

    // 第二步：申请临时缓冲区，先保存普通 Base64 结果。
    tmp = (char *)calloc(1, (size_t)base64_len + 1);
    if (tmp == NULL) {
        return -1;
    }

    // 第三步：调用 OpenSSL 完成普通 Base64 编码。
    EVP_EncodeBlock((unsigned char *)tmp, src, (int)src_len);

    // 第四步：再把普通 Base64 改写为 Base64URL 格式。
    base64_to_base64url(tmp);

    // 第五步：确认转换后的结果仍能放入目标缓冲区。
    if (strlen(tmp) + 1 > dst_size) {
        free(tmp);
        return -1;
    }

    // 第六步：把结果复制给调用方，并释放临时缓冲区。
    strcpy(dst, tmp);
    free(tmp);
    return 0;
}

/**
 * @brief  把 Base64URL 字符串解码成原始二进制数据
 * @param  src 输入的 Base64URL 字符串
 * @param  dst 输出参数，用来保存解码结果
 * @param  dst_size dst 缓冲区大小
 * @param  out_len 输出参数，用来返回真实解码字节数
 * @return 成功返回 0，失败返回 -1
 */
static int base64url_decode(const char *src, unsigned char *dst, size_t dst_size, size_t *out_len) {
    char base64_buf[1024] = {0};
    int decoded_len = 0;
    int pad_count = 0;
    size_t base64_len = 0;

    // 第一步：先把 Base64URL 恢复成普通 Base64 字符串。
    if (base64url_to_base64(src, base64_buf, sizeof(base64_buf)) != 0) {
        return -1;
    }

    // 第二步：记录恢复后的字符串长度，并做基础边界检查。
    base64_len = strlen(base64_buf);
    if (base64_len > dst_size * 2 + 4) {
        return -1;
    }

    // 第三步：调用 OpenSSL 完成 Base64 解码。
    decoded_len = EVP_DecodeBlock(dst, (unsigned char *)base64_buf, (int)base64_len);
    if (decoded_len < 0) {
        return -1;
    }

    // 第四步：统计结尾 '=' 的数量。
    // EVP_DecodeBlock 返回的长度包含补位造成的多余字节，需要手动扣除。
    if (base64_len >= 2 && base64_buf[base64_len - 1] == '=') {
        pad_count++;
    }
    if (base64_len >= 2 && base64_buf[base64_len - 2] == '=') {
        pad_count++;
    }

    // 第五步：确认真实解码长度未超过输出缓冲区。
    if ((size_t)(decoded_len - pad_count) > dst_size) {
        return -1;
    }

    // 第六步：回填真实解码长度。
    *out_len = (size_t)(decoded_len - pad_count);
    return 0;
}

/**
 * @brief  使用 HMAC-SHA256 计算签名
 * @param  data 待签名字符串
 * @param  sign 输出参数，用来保存签名二进制内容
 * @param  sign_len 输出参数，用来返回签名字节数
 * @return 成功返回 0，失败返回 -1
 */
static int hmac_sha256_sign(const char *data, unsigned char *sign, unsigned int *sign_len) {
    unsigned char *ret = NULL;

    // 使用当前全局密钥对输入字符串做 HMAC-SHA256 计算。
    // sign 保存二进制签名结果，sign_len 保存签名字节数。
    ret = HMAC(EVP_sha256(),
               g_jwt_secret,
               (int)strlen(g_jwt_secret),
               (const unsigned char *)data,
               strlen(data),
               sign,
               sign_len);

    if (ret == NULL) {
        return -1;
    }

    return 0;
}

/**
 * @brief  解析 payload JSON 中的固定字段
 * @param  payload_json 解码后的 payload JSON 字符串
 * @param  user_id 输出参数，用来返回用户 id
 * @param  user_name 输出参数，用来返回用户名
 * @param  user_name_size user_name 缓冲区大小
 * @param  exp_time 输出参数，用来返回过期时间
 * @return 成功返回 0，失败返回 -1
 */
static int parse_payload_json(const char *payload_json,
                              int *user_id,
                              char *user_name,
                              size_t user_name_size,
                              time_t *exp_time) {
    long long iat_value = 0;
    long long exp_value = 0;
    char name_buf[JWT_USER_NAME_LEN] = {0};
    int uid_value = 0;
    int ret = 0;

    // 当前项目的 payload 结构固定为：
    // {"uid":数字,"uname":"字符串","iat":数字,"exp":数字}
    // 因此这里使用 sscanf 按固定格式做解析。
    ret = sscanf(payload_json,
                 "{\"uid\":%d,\"uname\":\"%63[^\"]\",\"iat\":%lld,\"exp\":%lld}",
                 &uid_value,
                 name_buf,
                 &iat_value,
                 &exp_value);
    if (ret != 4) {
        return -1;
    }

    // 解析成功后，按调用方需要回填各个字段。
    if (user_id != NULL) {
        *user_id = uid_value;
    }

    if (user_name != NULL && user_name_size > 0) {
        strncpy(user_name, name_buf, user_name_size - 1);
        user_name[user_name_size - 1] = '\0';
    }

    if (exp_time != NULL) {
        *exp_time = (time_t)exp_value;
    }

    return 0;
}

/**
 * @brief  设置 JWT 签名密钥
 * @param  secret 新的签名密钥字符串
 * @return 成功返回 0，失败返回 -1
 */
int jwt_set_secret(const char *secret) {
    // 第一步：校验密钥字符串是否有效。
    if (secret == NULL || secret[0] == '\0') {
        return -1;
    }

    // 第二步：确认密钥长度没有超过全局缓冲区大小。
    if (strlen(secret) >= sizeof(g_jwt_secret)) {
        return -1;
    }

    // 第三步：更新全局签名密钥。
    strcpy(g_jwt_secret, secret);
    return 0;
}

/**
 * @brief  生成一个 HS256 JWT 字符串
 * @param  user_id 用户 id
 * @param  user_name 用户名
 * @param  expire_seconds 有效秒数
 * @param  token 输出参数，用来保存最终生成的 token
 * @param  token_size token 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_create_token(int user_id, const char *user_name, int expire_seconds, char *token, size_t token_size) {
    char header_b64[256] = {0};
    char payload_json[256] = {0};
    char payload_b64[512] = {0};
    char sign_input[768] = {0};
    char sign_b64[256] = {0};
    unsigned char sign_buf[JWT_SIGN_LEN] = {0};
    unsigned int sign_len = 0;
    time_t now_time = time(NULL);
    time_t exp_time = now_time + expire_seconds;

    // 第一步：校验输入参数。
    if (user_name == NULL || token == NULL || token_size == 0 || expire_seconds <= 0) {
        return -1;
    }

    // 第二步：编码固定 JWT 头部。
    if (base64url_encode((const unsigned char *)JWT_HEADER_JSON,
                         strlen(JWT_HEADER_JSON),
                         header_b64,
                         sizeof(header_b64)) != 0) {
        return -1;
    }

    // 第三步：构造 payload JSON。
    // 其中 iat 表示签发时间，exp 表示过期时间。
    snprintf(payload_json,
             sizeof(payload_json),
             "{\"uid\":%d,\"uname\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
             user_id,
             user_name,
             (long long)now_time,
             (long long)exp_time);

    // 第四步：对 payload 做 Base64URL 编码。
    if (base64url_encode((const unsigned char *)payload_json,
                         strlen(payload_json),
                         payload_b64,
                         sizeof(payload_b64)) != 0) {
        return -1;
    }

    // 第五步：拼接待签名字符串，格式为 header.payload。
    snprintf(sign_input, sizeof(sign_input), "%s.%s", header_b64, payload_b64);

    // 第六步：计算 HMAC-SHA256 签名。
    if (hmac_sha256_sign(sign_input, sign_buf, &sign_len) != 0) {
        return -1;
    }

    // 第七步：把二进制签名转成 Base64URL 字符串。
    if (base64url_encode(sign_buf, sign_len, sign_b64, sizeof(sign_b64)) != 0) {
        return -1;
    }

    // 第八步：按 header.payload.signature 形式拼出最终 token。
    if (snprintf(token, token_size, "%s.%s.%s", header_b64, payload_b64, sign_b64) >= (int)token_size) {
        return -1;
    }

    return 0;
}

/**
 * @brief  校验一个 JWT 字符串，并解析其中的用户信息
 * @param  token 待校验的 token
 * @param  user_id 输出参数，用来返回 token 中的用户 id
 * @param  user_name 输出参数，用来返回 token 中的用户名
 * @param  user_name_size user_name 缓冲区大小
 * @param  exp_time 输出参数，用来返回 token 中的过期时间
 * @return 校验通过返回 0，校验失败返回 -1
 */
int jwt_verify_token(const char *token, int *user_id, char *user_name, size_t user_name_size, time_t *exp_time) {
    char token_buf[1024] = {0};
    char *header_part = NULL;
    char *payload_part = NULL;
    char *sign_part = NULL;
    char sign_input[768] = {0};
    unsigned char expected_sign[JWT_SIGN_LEN] = {0};
    unsigned int expected_sign_len = 0;
    unsigned char recv_sign[JWT_SIGN_LEN] = {0};
    size_t recv_sign_len = 0;
    unsigned char payload_json[512] = {0};
    size_t payload_len = 0;
    time_t parsed_exp_time = 0;

    // 第一步：校验 token 字符串的基本有效性。
    if (token == NULL || token[0] == '\0') {
        return -1;
    }

    if (strlen(token) >= sizeof(token_buf)) {
        return -1;
    }

    // 第二步：复制 token 到临时缓冲区，后续需要使用 strtok 切分。
    strcpy(token_buf, token);

    // 第三步：按 '.' 分隔出 header、payload、signature 三段。
    header_part = strtok(token_buf, ".");
    payload_part = strtok(NULL, ".");
    sign_part = strtok(NULL, ".");

    if (header_part == NULL || payload_part == NULL || sign_part == NULL) {
        return -1;
    }

    if (strtok(NULL, ".") != NULL) {
        return -1;
    }

    // 第四步：重新拼接 header.payload，准备做签名校验。
    snprintf(sign_input, sizeof(sign_input), "%s.%s", header_part, payload_part);

    // 第五步：使用当前密钥重新计算期望签名。
    if (hmac_sha256_sign(sign_input, expected_sign, &expected_sign_len) != 0) {
        return -1;
    }

    // 第六步：把 token 中携带的签名从 Base64URL 解码回二进制。
    if (base64url_decode(sign_part, recv_sign, sizeof(recv_sign), &recv_sign_len) != 0) {
        return -1;
    }

    // 第七步：比较签名长度和签名内容。
    if (recv_sign_len != expected_sign_len) {
        return -1;
    }

    if (memcmp(recv_sign, expected_sign, expected_sign_len) != 0) {
        return -1;
    }

    // 第八步：把 payload 解码回 JSON 文本。
    if (base64url_decode(payload_part, payload_json, sizeof(payload_json) - 1, &payload_len) != 0) {
        return -1;
    }

    payload_json[payload_len] = '\0';

    // 第九步：解析 payload 中的用户信息和过期时间。
    if (parse_payload_json((char *)payload_json, user_id, user_name, user_name_size, &parsed_exp_time) != 0) {
        return -1;
    }

    // 第十步：校验 token 是否已经过期。
    if (parsed_exp_time < time(NULL)) {
        return -1;
    }

    // 第十一步：调用方需要过期时间时，再把解析结果返回出去。
    if (exp_time != NULL) {
        *exp_time = parsed_exp_time;
    }

    return 0;
}
