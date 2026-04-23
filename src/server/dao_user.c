#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dao_user.h"
#include "db_pool.h"
#include "log.h"

/**
 * @brief  根据用户名查询 users 表中的用户信息
 * @param  username 用户名
 * @param  out_id 输出参数，返回用户 id
 * @param  out_hash 输出参数，返回密码哈希
 * @param  out_salt 输出参数，返回盐值
 * @return 成功返回 0，失败返回 -1
 */
int dao_get_user_by_name(const char*username,int *out_id,char *out_hash,char *out_salt){
    char sql[512];
    snprintf(sql,sizeof(sql),"SELECT id,password_hash,salt FROM users WHERE username='%s'",username);

    // users.username 是唯一键，因此这里理论上最多只会返回一行。
    MYSQL_RES *res=db_execute_query(sql);
    if(res==NULL){
        LOG_ERROR("查询数据库出错");
        return -1;
    }

    MYSQL_ROW row=mysql_fetch_row(res);
    if(row!=NULL){
        // row[0] -> id
        // row[1] -> password_hash
        // row[2] -> salt
        *out_id=atoi(row[0]);
        strcpy(out_hash,row[1]);
        strcpy(out_salt,row[2]);
        mysql_free_result(res);//释放结果集
        return 0;
    }
    mysql_free_result(res);//释放结果集
    return -1;//用户不存在
}


/**
 * @brief  向 users 表插入一条新用户记录
 * @param  username 用户名
 * @param  password_hash 已经计算好的密码哈希
 * @param  salt 用户盐值
 * @return 成功返回 0，失败返回 -1
 */
int dao_insert_user(const char*username,const char*password_hash,const char*salt){
    char sql[512];
    snprintf(sql,sizeof(sql),"INSERT INTO users(username,password_hash,salt) VALUES('%s','%s','%s')",
             username,password_hash,salt);

    // 这里不直接处理“用户名重复”等业务语义。
    // DAO 层只负责把 SQL 交给数据库执行，业务层再根据返回值给客户端决定提示文案。
    return db_execute_update(sql);
}
