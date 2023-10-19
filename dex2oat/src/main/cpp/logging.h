#pragma once

#include <android/log.h>

#ifndef LOG_TAG
#define LOG_TAG "LSPosedDex2Oat"
#endif

#ifdef LOG_DISABLED
#define LOGD(...) 0
#define LOGV(...) 0
#define LOGI(...) 0
#define LOGW(...) 0
#define LOGE(...) 0
#else
#ifndef NDEBUG
// __FILE_NAME__: 获取文件名的宏
// __LINE__: 获取行号的宏
// __PRETTY_FUNCTION__: 获取函数签名的宏
// __VA_OPT__: 用于定义可变参数的宏和 __VA_ARGS__ 一起使用 
//__VA_OPT__(, ): 如果可变参数大于 0 个，则在可变参数前面插入逗号
#define LOGD(fmt, ...)                                                                             \
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,                                                \
                        "%s:%d#%s"                                                                 \
                        ": " fmt,                                                                  \
                        __FILE_NAME__, __LINE__, __PRETTY_FUNCTION__ __VA_OPT__(,) __VA_ARGS__)
#define LOGV(fmt, ...)                                                                             \
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG,                                              \
                        "%s:%d#%s"                                                                 \
                        ": " fmt,                                                                  \
                        __FILE_NAME__, __LINE__, __PRETTY_FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOGD(...) 0
#define LOGV(...) 0
#endif
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__)
#define PLOGE(fmt, args...) LOGE(fmt " failed with %d: %s", ##args, errno, strerror(errno))
#endif
