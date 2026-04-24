#ifndef _DB_POOL_H_
#define _DB_POOL_H_

#include <mysql/mysql.h>

/**
 * @brief  初始化数据库连接池
 * @param  host MySQL 主机地址
 * @param  user MySQL 用户名
 * @param  pwd MySQL 密码
 * @param  db_name 数据库名
 * @param  pool_size 连接池大小，也就是预先创建多少条数据库连接
 * @return 成功返回 0，失败返回 -1
 */
int init_db_pool(const char* host,const char* user,const char* pwd,
                 const char* db_name, int pool_size);

/**
 * @brief  销毁数据库连接池，并关闭池中的全部连接
 * @return 无
 */
void destroy_db_pool();

/**
 * @brief  执行一条增删改 SQL
 * @param  sql 要执行的 SQL 语句
 * @return 成功返回 0，失败返回 -1
 */
int db_execute_update(const char* sql);

/**
 * @brief  执行一条查询 SQL，并返回结果集
 * @param  sql 要执行的查询 SQL
 * @return 成功返回结果集指针，失败返回 NULL
 */
MYSQL_RES* db_execute_query(const char* sql);

#endif
