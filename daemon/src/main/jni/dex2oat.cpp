/*
 * This file is part of LSPosed.
 *
 * LSPosed is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LSPosed is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LSPosed.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2023 LSPosed Contributors
 */

#include <fcntl.h>
#include <jni.h>
#include <string>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

#include "logging.h"

extern "C"
JNIEXPORT void JNICALL
Java_org_lsposed_lspd_service_Dex2OatService_doMountNative(JNIEnv *env, jobject,
                                                           jboolean enabled,
                                                           jstring r32, jstring d32,
                                                           jstring r64, jstring d64) {
    char dex2oat32[PATH_MAX], dex2oat64[PATH_MAX];
    //realpath: 用于获取指定路径的绝对路径; 用于获取 dex2oat32 和 dex2oat64 的绝对路径
    realpath("bin/dex2oat32", dex2oat32);
    realpath("bin/dex2oat64", dex2oat64);

    if (pid_t pid = fork(); pid > 0) { // parent
        //阻塞进程，等待子进程状态变化（终止或停止）
        waitpid(pid, nullptr, 0);
    } else {
        // child pid = 0
        LOGD("子进程开始执行 mount...");
        //将当前进程加入到 PID 为 1 的进程所在的文件系统（mnt）命名空间; setns 需要 root 权限
        int ns = open("/proc/1/ns/mnt", O_RDONLY);
        setns(ns, CLONE_NEWNS);
        close(ns);

        const char *r32p, *d32p, *r64p, *d64p;
        if (r32) r32p = env->GetStringUTFChars(r32, nullptr);
        if (d32) d32p = env->GetStringUTFChars(d32, nullptr);
        if (r64) r64p = env->GetStringUTFChars(r64, nullptr);
        if (d64) d64p = env->GetStringUTFChars(d64, nullptr);

        if (enabled) {
            LOGI("Enable dex2oat wrapper");
            if (r32) {
                //mount: 将指定的文件系统挂载到指定的目录; 其中第一次 mount 会将 dex2oat32 挂载到 r32p 目录，第二次 mount 只修改 r32p 挂载点的权限为只读
                //两次 mount 虽然可以合并为一次，但是可能会出现挂载点读写权限不正确的问题
                LOGI("1 mount dex2oat32 to %s", r32p);
                mount(dex2oat64, r32p, nullptr, MS_BIND, nullptr);
                mount(nullptr, r32p, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr);
            }
            if (d32) {
                LOGI("2 mount dex2oat32 to %s", d32p);
                mount(dex2oat64, d32p, nullptr, MS_BIND, nullptr);
                mount(nullptr, d32p, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr);
            }
            if (r64) {
                LOGI("3 mount dex2oat64 to %s", r64p);
                mount(dex2oat64, r64p, nullptr, MS_BIND, nullptr);
                mount(nullptr, r64p, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr);

            }
            if (d64) {
                LOGI("4 mount dex2oat64 to %s", d64p);
                mount(dex2oat64, d64p, nullptr, MS_BIND, nullptr);
                mount(nullptr, d64p, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr);
            }
            execlp("resetprop", "resetprop", "--delete", "dalvik.vm.dex2oat-flags", nullptr);
        } else {
            LOGI("Disable dex2oat wrapper");
            if (r32) umount(r32p);
            if (d32) umount(d32p);
            if (r64) umount(r64p);
            if (d64) umount(d64p);
            //system.prop 文件里面也有 dalvik.vm.dex2oat-flags 的配置;
            //--inline-max-code-units=0: 禁用 inline
            execlp("resetprop", "resetprop", "dalvik.vm.dex2oat-flags", "--inline-max-code-units=0",
                   nullptr);
        }

        /**
         * execlp:
         *  当调用之后，当前进程的映像（代码段、数据段等）被新程序的映像替换，然后新程序就开始执行。所以实际上并不会"返回"到调用它的代码来继续执行。
         *  但是在发生错误的时候会返回，比如说新程序的文件没找到，或者没有执行权限等。当这些函数返回时，它们会返回-1，并设置全局变量errno来表示错误码
         *
         * 所以只有在执行失败的时候才会执行下面的代码
         */
        PLOGE("Failed to resetprop");
        exit(1);
    }
}

/**
 * 往当前进程的 socket 写入内容
 */
static int setsockcreatecon_raw(const char *context) {
    // /proc/self/task/[tid]/attr/sockcreate: 用于设置当前进程的 socket 创建上下文
    std::string path = "/proc/self/task/" + std::to_string(gettid()) + "/attr/sockcreate";
    LOGD("setsockcreatecon_raw: path = %s, pid = %d, context = %s", path.c_str(), getpid(), context);
    //O_RDWR: 读写方式打开; O_CLOEXEC: 在执行 exec 系统调用时，关闭文件描述符, 防止文件描述符泄露
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret;
    if (context) {
        do {
            //write: 将数据写入文件, 返回写入的字节数, 如果出错返回-1;  +1 是为了写入 '\0'
            ret = write(fd, context, strlen(context) + 1);
        } while (ret < 0 && errno == EINTR);//EINTR: 被信号中断; 写入失败后重试
    } else {
        do {
            ret = write(fd, nullptr, 0); // clear
        } while (ret < 0 && errno == EINTR);
    }
    close(fd);
    return ret < 0 ? -1 : 0;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_lsposed_lspd_service_Dex2OatService_setSockCreateContext(JNIEnv *env, jclass,
                                                                  jstring contextStr) {
    const char *context = env->GetStringUTFChars(contextStr, nullptr);
    int ret = setsockcreatecon_raw(context);
    env->ReleaseStringUTFChars(contextStr, context);
    return ret == 0;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_org_lsposed_lspd_service_Dex2OatService_getSockPath(JNIEnv *env, jobject) {
    //在安装的脚本里面会进行修改,并设置为随机值 @see customize.sh
    return env->NewStringUTF("5291374ceda0aef7c5d86cd2a4f6a3ac\0");
}
