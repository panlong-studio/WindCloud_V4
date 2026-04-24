#ifndef _DAO_USER_H_
#define _DAO_USER_H_

/**
 * @brief  根据用户名查询 users 表中的用户信息
 * @param  username 用户名
 * @param  out_id 输出参数，成功时返回用户 id
 * @param  out_hash 输出参数，成功时返回数据库中保存的密码哈希
 * @param  out_salt 输出参数，成功时返回数据库中保存的盐值
 * @return 成功返回 0，不存在或查询失败返回 -1
 */
int dao_get_user_by_name(const char*username,int *out_id,char *out_hash,char *out_salt);

/**
 * @brief  向 users 表插入一条新的用户记录
 * @param  username 用户名
 * @param  password_hash 已经计算好的密码哈希
 * @param  salt 与密码哈希配套使用的盐值
 * @return 成功返回 0，失败返回 -1
 */
int dao_insert_user(const char*username,const char*password_hash,const char*salt);

#endif
