#pragma once

#include <stdint.h>
#include <sys/types.h>

#include <android/log.h>

#define NS_PER_SEC 1000000000L// 1 秒 = 10^9 纳秒
#define MS_PER_NSEC 1000000
#define LOGGER_ENTRY_MAX_LEN (5 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AndroidLogEntry_t {
    time_t tv_sec;// 秒
    long tv_nsec;// 纳秒
    android_LogPriority priority;
    int32_t uid;
    int32_t pid;
    int32_t tid;
    const char *tag;
    size_t tagLen;
    size_t messageLen;
    const char *message;
} AndroidLogEntry;

struct logger_entry {
    uint16_t len;      /* length of the payload */
    uint16_t hdr_size; /* sizeof(struct logger_entry) */
    int32_t pid;       /* generating process's pid */
    uint32_t tid;      /* generating process's tid */
    uint32_t sec;      /* seconds since Epoch */
    uint32_t nsec;     /* nanoseconds */
    uint32_t lid;      /* log id of the payload, bottom 4 bits currently */
    uint32_t uid;      /* generating process's uid */
};

struct log_msg {
    union alignas(4) { // 4 字节对齐
        unsigned char buf[LOGGER_ENTRY_MAX_LEN + 1];
        struct logger_entry entry;
    };
#ifdef __cplusplus
    log_id_t id() {
        return static_cast<log_id_t>(entry.lid);
    }
#endif
};
struct logger;
struct logger_list;

long android_logger_get_log_size(struct logger* logger);// Android 系统提供的获取 logger 的大小的方法
int android_logger_set_log_size(struct logger *logger, unsigned long size);// Android 系统提供的设置 logger 的大小的方法
/**
 *  Android 系统提供的创建 logger_list 的方法
 * @param mode 表示日志模式，可以是 O_RDONLY、O_WRONLY 或 O_RDWR
 * @param tail 表示从日志的哪一行开始读取
 * @param pid 表示要读取的进程 ID，如果为 0，则表示读取所有进程的日志
 * @return 返回一个指向 logger_list 对象的指针; 如果创建失败，则返回 NULL
 */
struct logger_list *android_logger_list_alloc(int mode, unsigned int tail, pid_t pid);
void android_logger_list_free(struct logger_list *logger_list);// Android 系统提供的释放 logger_list 的方法
int android_logger_list_read(struct logger_list *logger_list, struct log_msg *log_msg);// Android 系统提供的读取 logger_list 的方法
struct logger *android_logger_open(struct logger_list *logger_list, log_id_t id);// Android 系统提供的打开 logger 的方法
int android_log_processLogBuffer(struct logger_entry *buf, AndroidLogEntry *entry);// Android 系统提供的处理 logger 的方法
#ifdef __cplusplus
}
#endif
