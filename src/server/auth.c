#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <time.h>
#include "auth.h"
#include "session.h"
#include "dao_user.h"
#include "log.h"

/**
 * @brief  把二进制数据转换成十六进制字符串
 * @param  bin 输入的二进制数据
 * @param  hex 输出参数，用来保存十六进制字符串
 * @param  len 输入二进制数据长度（字节数）
 * @return 无
 */
static void bin_to_hex(const unsigned char *bin,char *hex,int len){
    // 每个字节会被格式化成 2 个十六进制字符，例如 0x0f -> "0f"。
    for(int i=0;i<len;i++){
        sprintf(hex+i*2,"%02x",bin[i]);
    }
    hex[len*2]='\0';
}

/**
 * @brief  生成用户密码盐值
 * @param  salt 输出参数，用来保存 32 位十六进制盐值字符串
 * @return 无
 */
static void generate_salt(char*salt){
    unsigned char random_bytes[16];

    // 优先使用 OpenSSL 的安全随机数。
    // 如果极端情况下 RAND_bytes 不可用，再退化为普通随机数兜底，避免注册流程直接瘫痪。
    if(RAND_bytes(random_bytes, sizeof(random_bytes)) != 1){
        srand(time(NULL));
        for(int i=0;i<16;i++){
            random_bytes[i]=rand()%256;
        }
    }

    // 16 字节随机数据最终会被展开成 32 位十六进制字符串。
    bin_to_hex(random_bytes, salt, 16);
}

/**
 * @brief  计算“明文密码 + 盐值”的 SHA-256 哈希
 * @param  password 用户输入的明文密码
 * @param  salt 用户对应的盐值
 * @param  output 输出参数，用来保存 64 位十六进制哈希字符串
 * @return 无
 */
static void hash_password_with_salt(const char *password,const char*salt,char*output){
    char salted_password[256];
    unsigned char hash[SHA256_DIGEST_LENGTH];

    // 这里直接把“密码 + 盐值”拼起来再做 SHA-256。
    // 这样即使两个用户密码相同，只要盐值不同，最终落库的哈希也会不同。
    snprintf(salted_password, sizeof(salted_password), "%s%s", password, salt);
    SHA256((unsigned char*)salted_password, strlen(salted_password), hash);
    bin_to_hex(hash, output, SHA256_DIGEST_LENGTH);
}

/**
 * @brief  处理 register 命令，完成新用户注册
 * @param  client_fd 当前客户端套接字
 * @param  data 客户端发送的注册参数，格式为 "用户名/密码"
 * @param  user_id 输出参数，注册成功时返回新用户 id，失败返回 -1
 * @return 无
 */
void handle_register(int client_fd,const char*data,int *user_id){
    // 注册协议把“用户名/密码”塞在同一个字符串里，通过 / 分隔。
    // 这里直接就地切分，后续分别拿到用户名和密码。
    char *user_name=strtok((char*)data,"/");
    char *user_passwd=strtok(NULL,"\r\n\t");

    if(!user_name || !user_passwd){
        send_msg(client_fd,"错误：用户名或密码格式不正确");
        *user_id=-1;
        return;
    }

    // 第一步：为新用户生成盐值，并把“密码 + 盐值”转换成最终要落库的哈希。
    char salt[33];
    char password_hash[65];
    generate_salt(salt);
    hash_password_with_salt(user_passwd, salt, password_hash);

    // 第二步：尝试把新用户写入 users 表。
    if(dao_insert_user(user_name, password_hash, salt) == 0){
        int new_id;
        char tmp_hash[65], tmp_salt[33];

        // 第三步：注册成功后再查一次，拿到新用户 id 回传给上层。
        // 这里复用现有 DAO，避免额外维护“插入后取自增 id”的专门接口。
        if(dao_get_user_by_name(user_name, &new_id, tmp_hash, tmp_salt) == 0){
            *user_id = new_id;
        }
        send_msg(client_fd, "注册成功！请继续登录。");   
    }else{
        *user_id=-1;
        send_msg(client_fd, "注册失败");
    }
}


/**
 * @brief  处理 login 命令，校验用户名和密码
 * @param  client_fd 当前客户端套接字
 * @param  data 客户端发送的登录参数，格式为 "用户名/密码"
 * @param  user_id 输出参数，登录成功时返回用户 id，失败返回 -1
 * @return 无
 */
void handle_login(int client_fd,const char*data,int *user_id){
    // 登录参数和注册参数的协议格式一致，同样使用 / 分隔用户名和密码。
    char *user_name=strtok((char*)data,"/");
    char *user_passwd=strtok(NULL,"\r\n\t");

    if(!user_name || !user_passwd){
        send_msg(client_fd,"错误：用户名或密码格式不正确");
        *user_id=-1;
        return;
    }

    int db_user_id;
    char stored_hash[65];
    char salt[33];

    // 第一步：先按用户名查询数据库中的用户 id、盐值和历史哈希。
    if(dao_get_user_by_name(user_name,&db_user_id,stored_hash,salt)!=0){
        *user_id=-1;
        send_msg(client_fd,"用户名不存在");
        return;
    }

    // 第二步：用“用户输入密码 + 数据库盐值”重新计算哈希，再和库里的哈希比对。
    char computed_hash[65];
    hash_password_with_salt(user_passwd, salt, computed_hash);

    if(strcmp(computed_hash,stored_hash)==0){
        // 比对成功，说明密码正确，登录通过。
        *user_id=db_user_id;
        char msg[128];
        snprintf(msg, sizeof(msg), "登录成功！欢迎您，user_id=%d！", *user_id);
        send_msg(client_fd, msg);
    }else{
        // 比对失败，只返回“密码错误”，不泄露更多数据库细节。
        *user_id=-1;
        send_msg(client_fd,"密码错误");
    } 
}
