#include "logcat.h"

#include <jni.h>
#include <unistd.h>
#include <string>
#include <android/log.h>
#include <array>
#include <cinttypes>
#include <chrono>
#include <thread>
#include <functional>
#include <sys/system_properties.h>

using namespace std::string_view_literals;
using namespace std::chrono_literals;

constexpr size_t kMaxLogSize = 4 * 1024 * 1024;
constexpr size_t kLogBufferSize = 64 * 1024;

namespace {
    constexpr std::array<char, ANDROID_LOG_SILENT + 1> kLogChar = {
            /*ANDROID_LOG_UNKNOWN*/'?',
            /*ANDROID_LOG_DEFAULT*/ '?',
            /*ANDROID_LOG_VERBOSE*/ 'V',
            /*ANDROID_LOG_DEBUG*/ 'D',
            /*ANDROID_LOG_INFO*/'I',
            /*ANDROID_LOG_WARN*/'W',
            /*ANDROID_LOG_ERROR*/ 'E',
            /*ANDROID_LOG_FATAL*/ 'F',
            /*ANDROID_LOG_SILENT*/ 'S',
    };

    size_t ParseUint(const char *s) {
        if (s[0] == '\0') return -1;

        while (isspace(*s)) {//isspace: 判断是否为空格
            s++;
        }

        if (s[0] == '-') {
            return -1;
        }

        //判断进制
        int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
        char *end;
        auto result = strtoull(s, &end, base);//strtoull: 将字符串转换成无符号长整型数
        if (end == s) {//不是数字
            return -1;
        }
        //处理字符串后缀. 比如 :10k
        if (*end != '\0') {
            const char *suffixes = "bkmgtpe";
            const char *suffix;
            if ((suffix = strchr(suffixes, tolower(*end))) == nullptr ||
                __builtin_mul_overflow(result, 1ULL << (10 * (suffix - suffixes)), &result)) {
                //__builtin_mul_overflow: 用于检查两个整数相乘是否会导致溢出. 如果相乘不会导致溢出，则将结果存储在 result 指向的位置
                return -1;
            }
        }
        //判断是否超出 size_t 的范围
        if (std::numeric_limits<size_t>::max() < result) {
            return -1;
        }
        return static_cast<size_t>(result);
    }

    inline size_t GetByteProp(std::string_view prop, size_t def = -1) {
        std::array<char, PROP_VALUE_MAX> buf{};
        if (__system_property_get(prop.data(), buf.data()) < 0) return def;
        return ParseUint(buf.data());
    }

    inline std::string GetStrProp(std::string_view prop, std::string def = {}) {
        std::array<char, PROP_VALUE_MAX> buf{};
        if (__system_property_get(prop.data(), buf.data()) < 0) return def;
        return {buf.data()};
    }

    inline bool SetIntProp(std::string_view prop, int val) {
        auto buf = std::to_string(val);
        return __system_property_set(prop.data(), buf.data()) >= 0;
    }

    inline bool SetStrProp(std::string_view prop, std::string_view val) {
        return __system_property_set(prop.data(), val.data()) >= 0;
    }

}  // namespace

class UniqueFile : public std::unique_ptr<FILE, std::function<void(FILE *)>> {
    inline static deleter_type deleter = [](auto f) { f && f != stdout && fclose(f); };
public:
    explicit UniqueFile(FILE *f) : std::unique_ptr<FILE, std::function<void(FILE *)>>(f, deleter) {}

    UniqueFile(int fd, const char *mode) : UniqueFile(fd > 0 ? fdopen(fd, mode) : stdout) {};

    UniqueFile() : UniqueFile(stdout) {};
};

class Logcat {
public:
    explicit Logcat(JNIEnv *env, jobject thiz, jmethodID method) :
            env_(env), thiz_(thiz), refresh_fd_method_(method) {}

    [[noreturn]] void Run();

private:
    inline void RefreshFd(bool is_verbose);

    inline void Log(std::string_view str);

    void OnCrash(int err);

    void ProcessBuffer(struct log_msg *buf);

    static size_t PrintLogLine(const AndroidLogEntry &entry, FILE *out);

    void EnsureLogWatchDog();

    JNIEnv *env_;
    jobject thiz_;
    jmethodID refresh_fd_method_;

    UniqueFile modules_file_{};
    size_t modules_file_part_ = 0;
    size_t modules_print_count_ = 0;

    UniqueFile verbose_file_{};
    size_t verbose_file_part_ = 0;
    size_t verbose_print_count_ = 0;

    pid_t my_pid_ = getpid();

    bool verbose_ = true;
};

size_t Logcat::PrintLogLine(const AndroidLogEntry &entry, FILE *out) {
    if (!out) return 0;
    constexpr static size_t kMaxTimeBuff = 64;
    struct tm tm{};
    std::array<char, kMaxTimeBuff> time_buff{};

    auto now = entry.tv_sec;
    auto nsec = entry.tv_nsec;
    auto message_len = entry.messageLen;
    const auto *message = entry.message;
    if (now < 0) {//如果是负数, 则需要对其进行取反处理
        nsec = NS_PER_SEC - nsec;
    }
    //去掉 message 末尾的换行符
    if (message_len >= 1 && message[message_len - 1] == '\n') {
        --message_len;
    }
    //将 now 转换为本地时间,并存储在 tm 中
    localtime_r(&now, &tm);
    //将 tm 转换为字符串,并存储在 time_buff 中
    strftime(time_buff.data(), time_buff.size(), "%Y-%m-%dT%H:%M:%S", &tm);
    //注意: %-15.*s 和 %.*s 的意思, 他们需要两个参数.第一个参数 .* 表示动态长度; 第二个参数是一个字符串
    int len = fprintf(out, "[ %s.%03ld %8d:%6d:%6d %c/%-15.*s ] %.*s\n",
                      time_buff.data(),
                      nsec / MS_PER_NSEC,
                      entry.uid, entry.pid, entry.tid,
                      kLogChar[entry.priority], static_cast<int>(entry.tagLen),
                      entry.tag, static_cast<int>(message_len), message);
    fflush(out);
    // trigger overflow when failed to generate a new fd
    if (len <= 0) len = kMaxLogSize;
    return static_cast<size_t>(len);
}

/**
 * 将 end 的内容输出到上个文件中, 然后获取一个新的日志文件,在文件头部写入 start
 */
void Logcat::RefreshFd(bool is_verbose) {
    constexpr auto start = "----part %zu start----\n";
    constexpr auto end = "-----part %zu end----\n";
    if (is_verbose) {
        verbose_print_count_ = 0;
        fprintf(verbose_file_.get(), end, verbose_file_part_);
        fflush(verbose_file_.get());
        verbose_file_ = UniqueFile(env_->CallIntMethod(thiz_, refresh_fd_method_, JNI_TRUE), "a");
        verbose_file_part_++;
        fprintf(verbose_file_.get(), start, verbose_file_part_);
        fflush(verbose_file_.get());
    } else {
        modules_print_count_ = 0;
        fprintf(modules_file_.get(), end, modules_file_part_);
        fflush(modules_file_.get());
        modules_file_ = UniqueFile(env_->CallIntMethod(thiz_, refresh_fd_method_, JNI_FALSE), "a");
        modules_file_part_++;
        fprintf(modules_file_.get(), start, modules_file_part_);
        fflush(modules_file_.get());
    }
}

inline void Logcat::Log(std::string_view str) {
    if (verbose_) {
        fprintf(verbose_file_.get(), "%.*s", static_cast<int>(str.size()), str.data());
        fflush(verbose_file_.get());
    }
    fprintf(modules_file_.get(), "%.*s", static_cast<int>(str.size()), str.data());
    fflush(modules_file_.get());
}

void Logcat::OnCrash(int err) {
    using namespace std::string_literals;
    constexpr size_t max_restart_logd_wait = 1U << 10;
    static size_t kLogdCrashCount = 0;
    static size_t kLogdRestartWait = 1 << 3;
    if (++kLogdCrashCount >= kLogdRestartWait) {
        Log("\nLogd crashed too many times, trying manually start...\n");
        __system_property_set("ctl.restart", "logd");
        if (kLogdRestartWait < max_restart_logd_wait) {
            kLogdRestartWait <<= 1;
        } else {
            kLogdCrashCount = 0;
        }
    } else {
        Log("\nLogd maybe crashed (err="s + strerror(err) + "), retrying in 1s...\n");
    }

    std::this_thread::sleep_for(1s);
}

void Logcat::ProcessBuffer(struct log_msg *buf) {
    AndroidLogEntry entry;
    if (android_log_processLogBuffer(&buf->entry, &entry) < 0) return;

    entry.tagLen--;

    //tag: 标签. 第二个参数表示标签的长度
    std::string_view tag(entry.tag, entry.tagLen);
    bool shortcut = false;
    //如果是 LSPosed-Bridge 或 XSharedPreferences 的日志, 则输出到 modules_file_ 中
    if (tag == "LSPosed-Bridge"sv || tag == "XSharedPreferences"sv || tag == "LSPosedContext") [[unlikely]] {
        modules_print_count_ += PrintLogLine(entry, modules_file_.get());
        shortcut = true;
    }
    if (verbose_ && (shortcut || buf->id() == log_id::LOG_ID_CRASH ||
                     entry.pid == my_pid_ || tag == "Magisk"sv || tag == "Dobby"sv ||
                     tag.starts_with("Riru"sv) || tag.starts_with("zygisk"sv) ||
                     tag == "LSPlant"sv || tag.starts_with("LSPosed"sv))) [[unlikely]] {
        //如果是 LOG_ID_CRASH 或者是 Magisk\Dobby\Riru\zygisk\LSPlant\LSPosed 的日志, 则输出到 verbose_file_ 中
        verbose_print_count_ += PrintLogLine(entry, verbose_file_.get());
    }
    //如果是 LSPosedLogcat 的日志, 会根据日志内容进行一些简单的日志输出控制
    if (entry.pid == my_pid_ && tag == "LSPosedLogcat"sv) [[unlikely]] {
        std::string_view msg(entry.message, entry.messageLen);
        if (msg == "!!start_verbose!!"sv) {
            verbose_ = true;
            verbose_print_count_ += PrintLogLine(entry, verbose_file_.get());
        } else if (msg == "!!stop_verbose!!"sv) {
            verbose_ = false;
        } else if (msg == "!!refresh_modules!!"sv) {
            RefreshFd(false);
        } else if (msg == "!!refresh_verbose!!"sv) {
            RefreshFd(true);
        }
    }
}

/**
 * 用于监控系统日志相关的属性，并在必要时重新设置日志缓存大小和日志的输出级别
*/
void Logcat::EnsureLogWatchDog() {
    constexpr static auto kLogdSizeProp = "persist.logd.size"sv;
    constexpr static auto kLogdTagProp = "persist.log.tag"sv;//设置日志的输出级别
    constexpr static auto kLogdMainSizeProp = "persist.logd.size.main"sv;
    constexpr static auto kLogdCrashSizeProp = "persist.logd.size.crash"sv;
    constexpr static size_t kErr = -1;
    std::thread watch_dog([this] {
        while (true) {
            auto logd_size = GetByteProp(kLogdSizeProp);
            auto logd_tag = GetStrProp(kLogdTagProp);
            auto logd_main_size = GetByteProp(kLogdMainSizeProp);
            auto logd_crash_size = GetByteProp(kLogdCrashSizeProp);
            if (!logd_tag.empty() ||
                !((logd_main_size == kErr && logd_crash_size == kErr && logd_size != kErr &&
                   logd_size >= kLogBufferSize) ||
                  (logd_main_size != kErr && logd_main_size >= kLogBufferSize &&
                   logd_crash_size != kErr &&
                   logd_crash_size >= kLogBufferSize))) {
                SetIntProp(kLogdSizeProp, std::max(kLogBufferSize, logd_size));
                SetIntProp(kLogdMainSizeProp, std::max(kLogBufferSize, logd_main_size));
                SetIntProp(kLogdCrashSizeProp, std::max(kLogBufferSize, logd_crash_size));
                SetStrProp(kLogdTagProp, "");
                SetStrProp("ctl.start", "logd-reinit");//启动 logd-reinit 服务
            }
            const auto *pi = __system_property_find(kLogdTagProp.data());
            uint32_t serial = 0;// 将 kLogdTagProp 属性的值读取到 serial 中
            if (pi != nullptr) {
                __system_property_read_callback(pi, [](auto *c, auto, auto, auto s) {
                    *reinterpret_cast<uint32_t *>(c) = s;
                }, &serial);
            }
            //会阻塞在这里,直到 kLogdTagProp 属性被修改
            if (!__system_property_wait(pi, serial, &serial, nullptr)) break;
            if (pi != nullptr){ 
                Log("\nResetting log settings\n");
            } else{
                Log("\nWatchdog sleep 1s\n");
                std::this_thread::sleep_for(1s);
            }
            // log tag prop was not found; to avoid frequently trigger wait, sleep for a while
        }
    });
    pthread_setname_np(watch_dog.native_handle(), "watchdog");//设置线程名
    watch_dog.detach();//detach: 分离线程,不需要等待线程结束
}

void Logcat::Run() {
    //constexpr: 常量表达式
    constexpr size_t tail_after_crash = 10U;//U: unsigned int
    size_t tail = 0;
    RefreshFd(true);
    RefreshFd(false);

    EnsureLogWatchDog();

    while (true) {
        //decltype: 获取表达式的类型
        std::unique_ptr<logger_list, decltype(&android_logger_list_free)> logger_list{
                android_logger_list_alloc(0, tail, 0), &android_logger_list_free};
        tail = tail_after_crash;

        // 遍历 LOG_ID_MAIN 和 LOG_ID_CRASH, 检测日志缓存大小是否小于 kLogBufferSize, 如果小于则设置为 kLogBufferSize
        for (log_id id:{LOG_ID_MAIN, LOG_ID_CRASH}) {
            auto *logger = android_logger_open(logger_list.get(), id);
            if (logger == nullptr) continue;
            if (auto size = android_logger_get_log_size(logger);
                    size >= 0 && static_cast<size_t>(size) < kLogBufferSize) {
                android_logger_set_log_size(logger, kLogBufferSize);
            }
        }

        struct log_msg msg{};

        while (true) {
            //unlikely: 表示这个分支的执行概率很小
            if (android_logger_list_read(logger_list.get(), &msg) <= 0) [[unlikely]] break;

            ProcessBuffer(&msg);

            //如果日志文件的大小超过了 kMaxLogSize, 则重新获取一个新的日志文件
            if (verbose_print_count_ >= kMaxLogSize) [[unlikely]] RefreshFd(true);
            if (modules_print_count_ >= kMaxLogSize) [[unlikely]] RefreshFd(false);
        }

        OnCrash(errno);
    }
}

extern "C"
JNIEXPORT void JNICALL
// NOLINTNEXTLINE
Java_org_lsposed_lspd_service_LogcatService_runLogcat(JNIEnv *env, jobject thiz) {
    jclass clazz = env->GetObjectClass(thiz);
    jmethodID method = env->GetMethodID(clazz, "refreshFd", "(Z)I");
    Logcat logcat(env, thiz, method);
    logcat.Run();
}
