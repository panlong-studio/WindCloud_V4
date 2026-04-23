#ifndef __ERROR_CHECK_H__
#define __ERROR_CHECK_H__

#include <errno.h>
#include <stdlib.h>
#include <string.h>  
#include "log.h" 

#define ARGS_CHECK(argc, expected) \
    do { \
        if ((argc) != (expected)) { \
            LOG_ERROR("参数个数错误，期望=%d，实际=%d", expected, argc); \
            exit(1); \
        } \
    } while (0)


#define ERROR_CHECK(ret, error_flag, msg) \
    do { \
        if ((ret) == (error_flag)) { \
            LOG_ERROR("%s失败，错误码=%d", msg, errno); \
            exit(1); \
        } \
    } while (0)

#define THREAD_ERROR_CHECK(ret, msg) \
    do { \
        if (0 != (ret)) { \
            LOG_ERROR("%s失败，错误码=%d", msg, ret); \
            exit(1); \
        } \
    } while (0)

#endif // __ERROR_CHECK_H__
