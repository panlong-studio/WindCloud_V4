#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "db_pool.h"
#include "log.h"

#define MAX_POOL_SIZE 10

//======连接池内部结构（对外隐藏）=======
static MYSQL* conn_pool[MAX_POOL_SIZE];
static int pool_top = -1; //栈顶指针 -1 表示空池
static int g_pool_size = 0;

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_cond = PTHREAD_COND_INITIALIZER;

/**
 * @brief  从连接池中取出一个可用的 MySQL 连接
 * @return 成功返回 MYSQL 连接指针
 */
static MYSQL* get_connection(){
    pthread_mutex_lock(&pool_lock);

    // 如果连接池空了，当前线程就阻塞等待。
    // 直到别的线程释放连接并发出条件变量通知，这里才会继续往下执行。
    while(pool_top==-1){
        pthread_cond_wait(&pool_cond,&pool_lock);
    }

    // 连接池内部用“栈”的方式管理连接：
    // 1. 取连接时从栈顶弹出
    // 2. 还连接时再压回栈顶
    MYSQL* conn=conn_pool[pool_top];
    pool_top--;//出栈

    pthread_mutex_unlock(&pool_lock);
    return conn;
}

/**
 * @brief  把一个 MySQL 连接归还到连接池
 * @param  conn 要归还的 MYSQL 连接指针
 * @return 无
 */
static void release_connection(MYSQL *conn){
    if(conn==NULL) return;

    pthread_mutex_lock(&pool_lock);

    // 把连接重新压回栈顶，供后续工作线程复用。
    pool_top++;
    conn_pool[pool_top]=conn;//入栈

    pthread_cond_signal(&pool_cond);//通知等待的线程有连接可用
    pthread_mutex_unlock(&pool_lock);
}
//===================================


//======对外接口实现========

/**
 * @brief  初始化数据库连接池
 * @param  host MySQL 主机地址
 * @param  user MySQL 用户名
 * @param  pwd MySQL 密码
 * @param  db_name 数据库名
 * @param  pool_size 期望连接池大小
 * @return 成功返回 0，失败返回 -1
 */
int init_db_pool(const char* host,const char*user,const char*pwd,
                 const char*db_name,int pool_size){
    // 限制连接池大小，防止调用方传入过大值导致数组越界或资源耗尽。
    if(pool_size>MAX_POOL_SIZE){
        pool_size=MAX_POOL_SIZE;
    }

    g_pool_size=pool_size;

    // 启动阶段一次性创建好所有数据库连接。
    // 后续业务线程只负责“拿连接 / 还连接”，不再临时新建连接。
    for(int i=0;i<pool_size;i++){
        MYSQL *conn=mysql_init(NULL);
        if(conn==NULL){
            LOG_ERROR("db_pool:MySQL 句柄初始化失败");
            return -1;
        }

        // 开启自动重连机制这一版暂时没有打开，避免和当前教学代码的行为不一致。
        // 如果后续要增强稳定性，可以再评估是否启用 MYSQL_OPT_RECONNECT。

        // 真正连接到目标数据库。
        if(mysql_real_connect(conn,host,user,pwd,db_name,0,NULL,0)==NULL){
            LOG_ERROR("db_pool:MySQL 连接失败: %s",mysql_error(conn));
            mysql_close(conn);
            return -1;
        }

        // 初始化阶段就把连接全部压入连接池。
        pool_top++;
        conn_pool[pool_top]=conn;
    }

    LOG_INFO("db_pool:数据库连接池初始化成功，池大小=%d",pool_size);
    return 0;
}

/**
 * @brief  销毁数据库连接池并关闭所有连接
 * @return 无
 */
void destroy_db_pool(){
    pthread_mutex_lock(&pool_lock);

    // 当前实现里，池中连接都保存在 0..pool_top 范围内。
    for(int i=0;i<=pool_top;i++){
        mysql_close(conn_pool[i]);
    }
    pool_top=-1;
    pthread_mutex_unlock(&pool_lock);
    LOG_INFO("db_pool:数据库连接池销毁成功");
}

/**
 * @brief  执行增删改 SQL
 * @param  sql 要执行的 SQL 语句
 * @return 成功返回 0，失败返回 -1
 */
int db_execute_update(const char* sql){
    MYSQL *conn=get_connection();
    int ret=mysql_query(conn,sql);
    if(ret!=0){
        LOG_ERROR("db_pool:执行 SQL 失败: %s, SQL: %s",mysql_error(conn),sql);
        release_connection(conn);
        return -1;
    }
    release_connection(conn);
    return 0;
}

/**
 * @brief  执行查询 SQL 并返回结果集
 * @param  sql 要执行的查询 SQL
 * @return 成功返回 MYSQL_RES 结果集指针，失败返回 NULL
 */
MYSQL_RES *db_execute_query(const char* sql){
    MYSQL *conn=get_connection();
    if(mysql_query(conn,sql)!=0){
        LOG_ERROR("db_pool:执行 SQL 失败: %s, SQL: %s",mysql_error(conn),sql);
        release_connection(conn);
        return NULL;
    }

    // mysql_store_result 会把当前结果集完整取回到客户端内存中。
    // 这样后面即使先释放连接，调用方仍然可以继续安全读取 MYSQL_RES。
    MYSQL_RES *res=mysql_store_result(conn);

    release_connection(conn);

    return res;
}
