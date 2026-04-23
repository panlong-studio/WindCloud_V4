#ifndef _DAO_USER_H_
#define _DAO_USER_H_

//查询用户：成功返回0，并将id、hash、salt填入指针，不存在或失败返回-1
int dao_get_user_by_name(const char*username,int *out_id,char *out_hash,char *out_salt);

//插入新用户：成功返回0，失败返回-1
int dao_insert_user(const char*username,const char*password_hash,const char*salt);

#endif