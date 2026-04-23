#ifndef __LOG_H__
#define __LOG_H__

/*
*   日志系统设计：
*   1.日志级别定义：定义不同的日志级别（DEBUG, INFO, WARN, ERROR）
*
*   2.使用init_log函数初始化日志系统，设置日志级别和输出目标(填入日志文件名或NULL表示输出到控制台)
*   3.日志宏接口：（使用方法类似printf，支持可变参数）
*           LOG_DEBUG(fmt, ...)
*           LOG_INFO(fmt, ...)
*           LOG_WARN(fmt, ...)
*           LOG_ERROR(fmt, ...)
*     日志输出格式：[时间] [级别] [文件:行号 函数名] 消息内容
*   4.使用close_log函数关闭日志系统，释放资源
*/


#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>
#include <pthread.h> 

/*----日志级别定义----*/
#define LOG_LEVEL_DEBUG 0 //调试级别，输出详细的调试信息
#define LOG_LEVEL_INFO 1 //信息级别，输出一般的运行信息
#define LOG_LEVEL_WARN 2 //警告级别，输出可能的问题或异常情况
#define LOG_LEVEL_ERROR 3 //错误级别，输出严重错误信息

/*----用户接口----*/

//初始化日志系统
//----level_str 初始日志级别字符串 (如 "DEBUG", "INFO" 等)
//----log_file  日志保存路径，若为 NULL 则输出到控制台
int init_log(const char* level_str, const char* log_file);


//关闭日志系统
void close_log();

//核心日志写入函数 (供宏调用)
//----level 日志级别
//----file 触发日志的源文件名 用 __FILE__ 获取
//----line 触发日志的行号 用 __LINE__ 获取
//----func 触发日志的函数名 用 __FUNCTION__ 获取
//----fmt  日志消息格式字符串，类似 printf 的格式
//----...  可变参数列表，对应 fmt 中的格式说明符
void log_write(int level, const char* file, int line, const char* func, 
               const char* fmt, ...);


//宏接口
#define LOG_DEBUG(fmt, ...) do{log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__);}while(0)
#define LOG_INFO(fmt, ...)  do{log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__);}while(0)
#define LOG_WARN(fmt, ...)  do{log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__);}while(0)
#define LOG_ERROR(fmt, ...) do{log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__);}while(0)

#endif 