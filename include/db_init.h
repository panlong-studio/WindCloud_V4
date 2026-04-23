#ifndef DB_INIT_H
#define DB_INIT_H

// 初始化数据库和表
// 成功返回 0，失败返回 -1
int init_database(const char* host, const char* user, const char* pwd, const char* db_name);

#endif