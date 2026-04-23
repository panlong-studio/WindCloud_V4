#include "log.h"

#include <errno.h>

// g_log_level 表示当前全局日志级别，默认从 INFO 开始。
static int g_log_level=LOG_LEVEL_INFO;

// g_log_fp 表示当前日志输出到哪里。
// 如果是 NULL，说明还没有初始化。
static FILE* g_log_fp=NULL;

// g_log_mutex 用来保护多线程同时写日志。
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief  把字符串日志级别转换成内部整数级别
 * @param  level_str 日志级别字符串，例如 "DEBUG"、"INFO"
 * @return 成功返回对应整数级别，无法识别时默认返回 INFO
 */
static int level_to_int(const char* level_str){
    if(strcasecmp(level_str,"DEBUG")==0) return LOG_LEVEL_DEBUG;
    if(strcasecmp(level_str,"INFO")==0)  return LOG_LEVEL_INFO;
    if(strcasecmp(level_str,"WARN")==0)  return LOG_LEVEL_WARN;
    if(strcasecmp(level_str,"ERROR")==0) return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}

/**
 * @brief  把整数日志级别转换成字符串名称
 * @param  level 整数日志级别
 * @return 返回对应的字符串常量
 */
static const char* level_to_name(int level){
    switch(level){
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

/**
 * @brief  初始化日志系统
 * @param  level_str 初始日志级别字符串
 * @param  log_file 日志文件路径；如果传 NULL，则输出到控制台
 * @return 成功返回 0，失败返回 -1
 */
int init_log(const char* level_str, const char* log_file){
    // 如果调用者传了日志级别字符串，就把它转换成内部级别。
    if(level_str){
        g_log_level=level_to_int(level_str);
    }
    
    // 如果传了日志文件路径，就打开文件。
    if(log_file){
        // 采用追加模式打开，这样多次启动进程时不会覆盖旧日志。
        g_log_fp=fopen(log_file,"a");
        if(!g_log_fp){
            fprintf(stderr, "打开日志文件失败，错误码=%d\n", errno);
            return -1;
        }
    }else{
        // 否则默认输出到标准输出。
        g_log_fp=stdout;
    }
    return 0;
}


/**
 * @brief  关闭日志系统并释放相关资源
 * @return 无
 */
void close_log(){
    // 如果日志输出的是普通文件，就关闭它。
    // 如果输出的是 stdout，就不需要 fclose(stdout)。
    if(g_log_fp && g_log_fp!=stdout){
        fclose(g_log_fp);
    }

    // 销毁互斥锁。
    pthread_mutex_destroy(&g_log_mutex);
}


/**
 * @brief  按统一格式写一条日志
 * @param  level 本条日志的级别
 * @param  file 调用日志的源文件名
 * @param  line 调用日志的源代码行号
 * @param  func 调用日志的函数名
 * @param  fmt 格式字符串
 * @param  ... 可变参数列表
 * @return 无
 */
void log_write(int level, const char* file, int line, const char* func, const char* fmt, ...){
    // 如果本条日志级别低于当前全局级别，就直接丢弃。
    if(level<g_log_level){
        return;
    }

    // 再做一层兜底保护。
    // 即使上层忘了先 init_log，这里也回退到 stdout。
    if(g_log_fp==NULL){
        g_log_fp=stdout;
    }

    // 获取当前时间戳，并把它转换成可读文本。
    time_t now=time(NULL);

    // 把时间戳转换成本地时间结构体。
    struct tm* tm_info=localtime(&now);

    // time_str 保存格式化后的时间文本。
    char time_str[64];

    // 按“年-月-日 时:分:秒”格式输出时间。
    strftime(time_str,sizeof(time_str),"%Y-%m-%d %H:%M:%S",tm_info);
    
    // log_msg 用来保存格式化后的日志正文。
    char log_msg[1024];

    // args 用来接收 ... 可变参数。
    va_list args;

    // 初始化可变参数处理。
    va_start(args,fmt);

    // 根据 fmt 和 args，把最终日志正文拼到 log_msg 中。
    vsnprintf(log_msg,sizeof(log_msg),fmt,args);

    // 可变参数使用结束。
    va_end(args);
    
    // 加锁，避免多个线程同时写日志导致内容交叉。
    pthread_mutex_lock(&g_log_mutex);

    // 按统一格式写日志。
    // 统一输出“时间 + 级别 + 源码位置 + 正文”，方便后续排查跨模块链路问题。
    fprintf(g_log_fp,"[%s] [%s] [%s:%d %s] %s\n",time_str,level_to_name(level),file,line,func,log_msg);

    // fflush 让日志尽快真正落到输出设备上。
    fflush(g_log_fp);

    // 写完后解锁。
    pthread_mutex_unlock(&g_log_mutex);
}

               
