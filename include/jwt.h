#ifndef _JWT_H_
#define _JWT_H_

#include <stddef.h>
#include <time.h>

/* JWT payload 中用户名缓冲区大小。 */
#define JWT_USER_NAME_LEN 64
/* JWT 签名密钥缓冲区大小。 */
#define JWT_SECRET_LEN 128

/**
 * @brief  设置 JWT 签名密钥
 * @param  secret 新的签名密钥字符串
 * @return 成功返回 0，失败返回 -1
 */
int jwt_set_secret(const char *secret);

/**
 * @brief  生成一个 HS256 JWT 字符串
 * @param  user_id 用户 id
 * @param  user_name 用户名
 * @param  expire_seconds 有效秒数
 * @param  token 输出参数，用来保存最终生成的 token
 * @param  token_size token 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int jwt_create_token(int user_id, const char *user_name, int expire_seconds, char *token, size_t token_size);

/**
 * @brief  校验一个 JWT 字符串，并解析其中的用户信息
 * @param  token 待校验的 token
 * @param  user_id 输出参数，用来返回 token 中的用户 id
 * @param  user_name 输出参数，用来返回 token 中的用户名
 * @param  user_name_size user_name 缓冲区大小
 * @param  exp_time 输出参数，用来返回 token 中的过期时间
 * @return 校验通过返回 0，校验失败返回 -1
 */
int jwt_verify_token(const char *token, int *user_id, char *user_name, size_t user_name_size, time_t *exp_time);

#endif
