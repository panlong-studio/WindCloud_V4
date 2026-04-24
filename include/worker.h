#ifndef _WORKER_H_
#define _WORKER_H_

/**
 * @brief  工作线程入口函数
 * @param  arg 线程启动参数，实际类型为 worker_arg_t
 * @return 线程退出时返回 NULL
 */
void* thread_func(void* arg);

#endif
