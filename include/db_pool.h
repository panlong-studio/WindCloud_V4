#ifndef _DB_POOL_H_
#define _DB_POOL_H_

#include <mysql/mysql.h>

//初始化数据库连接池
// 参数：host, user, pwd, db_name, pool_size(预先创建几个连接，比如 10 个)
// 返回：0 成功，-1 失败
int init_db_pool(const char* host,const char* user,const char* pwd,
                 const char* db_name, int pool_size);

//销毁连接池（服务端退出时调用）
void destroy_db_pool();

//=========业务接口========
//执行增删改（insert, update, delete）
//返回：0 成功，-1 失败
int db_execute_update(const char* sql);

//执行查询（select）
//返回：查询结果集指针，失败返回 NULL
MYSQL_RES* db_execute_query(const char* sql);

#endif