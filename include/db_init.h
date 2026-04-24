#ifndef DB_INIT_H
#define DB_INIT_H

/**
 * @brief  初始化数据库与核心数据表
 * @param  host MySQL 主机地址
 * @param  user MySQL 用户名
 * @param  pwd MySQL 密码
 * @param  db_name 要初始化的数据库名
 * @return 成功返回 0，失败返回 -1
 */
int init_database(const char* host, const char* user, const char* pwd, const char* db_name);

#endif
