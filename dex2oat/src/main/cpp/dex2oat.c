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
 * Copyright (C) 2022 LSPosed Contributors
 */

//
// Created by Nullptr on 2022/4/1.
//

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "logging.h"

// todo 感觉没必要定义成宏,完全可以直接定义成一个变量,之后就没必要再判断了 ?
#if defined(__LP64__)
# define LP_SELECT(lp32, lp64) lp64
#else
# define LP_SELECT(lp32, lp64) lp32
#endif

#define ID_VEC(is64, is_debug) (((is64) << 1) | (is_debug))

// 用于存放 dex2oatd 的 socket 文件,在安装 lsp 后会被替换
const char kSockName[] = "5291374ceda0aef7c5d86cd2a4f6a3ac\0";

static ssize_t xrecvmsg(int sockfd, struct msghdr *msg, int flags) {
    int rec = recvmsg(sockfd, msg, flags);
    if (rec < 0) {
        PLOGE("recvmsg");
    }
    return rec;
}

static void *recv_fds(int sockfd, char *cmsgbuf, size_t bufsz, int cnt) {
    struct iovec iov = {
            .iov_base = &cnt,
            .iov_len  = sizeof(cnt),
    };
    struct msghdr msg = {
            .msg_iov        = &iov,
            .msg_iovlen     = 1,
            .msg_control    = cmsgbuf,
            .msg_controllen = bufsz
    };

    // MSG_WAITALL: 等待所有数据到达
    xrecvmsg(sockfd, &msg, MSG_WAITALL);
    // CMSG_FIRSTHDR: 获取第一个 cmsg
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    if (msg.msg_controllen != bufsz ||
            cmsg == NULL ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int) * cnt) ||
            cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS) {
        return NULL;
    }

    // CMSG_DATA: 获取 cmsg 中的数据
    return CMSG_DATA(cmsg);
}

/**
 * 从 socket 中读取一个文件描述符
*/
static int recv_fd(int sockfd) {
    // CMSG_SPACE: 计算出一个 cmsg 需要的空间大小
    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    void *data = recv_fds(sockfd, cmsgbuf, sizeof(cmsgbuf), 1);
    if (data == NULL)
        return -1;

    int result;
    // 从 data 中读取一个文件描述符
    memcpy(&result, data, sizeof(int));
    return result;
}

static int read_int(int fd) {
    int val;
    if (read(fd, &val, sizeof(val)) != sizeof(val))
        return -1;
    return val;
}

static void write_int(int fd, int val) {
    if (fd < 0) return;
    write(fd, &val, sizeof(val));
}

/**
 * dex2oat 处理的入口
 * 在应用列表选择 “重新优化” 会调用这个方法
 * @see #org.lsposed.lspd.service.PackageService.performDexOptMode
*/
int main(int argc, char **argv) {
    LOGD("dex2oat wrapper ppid=%d", getppid());
    struct sockaddr_un sock = {};
    // AF_UNIX: 本地通信
    sock.sun_family = AF_UNIX;
    strlcpy(sock.sun_path + 1, kSockName, sizeof(sock.sun_path) - 1);
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    size_t len = sizeof(sa_family_t) + strlen(sock.sun_path + 1) + 1;
    if (connect(sock_fd, (struct sockaddr *) &sock, len)) {
        PLOGE("failed to connect to %s", sock.sun_path + 1);
        return 1;
    }
    //写入的值对应的是文件描述符的位置,在 dex2oat.cpp 中会处理,然后返回对应的 dex2oat 文件描述符
    //判断是否为 isDebug: strstr(argv[0], "dex2oatd") != NULL 为 true 时，表示是 dex2oatd 进程
    write_int(sock_fd, ID_VEC(LP_SELECT(0, 1), strstr(argv[0], "dex2oatd") != NULL));
    int stock_fd = recv_fd(sock_fd);
    read_int(sock_fd);
    close(sock_fd);
    LOGD("sock: %s %d", sock.sun_path + 1, stock_fd);

    //根据获取的文件描述符 stock_fd, 执行优化命令
    const char *new_argv[argc + 2];
    for (int i = 0; i < argc; i++) new_argv[i] = argv[i];
    new_argv[argc] = "--inline-max-code-units=0";
    new_argv[argc + 1] = NULL;
    //打印 new_argv
    for (int i = 0; i < argc + 1; i++) {
        LOGD("argv[%d]: %s", i, new_argv[i]);
    }

//    //打印 environ
//    for (int i = 0; environ[i]; i++) LOGD("environ[%d]: %s", i, environ[i]);
    // environ: 当前的环境变量
    fexecve(stock_fd, (char **) new_argv, environ);
    PLOGE("fexecve failed");
    return 2;
}

/**
 * new_argv 的值
 *
 * argv[0]: /apex/com.android.art/bin/dex2oat64
 * argv[1]: --zip-fd=7
 * argv[2]: --zip-location=base.apk
 * argv[3]: --oat-fd=8
 * argv[4]: --oat-location=/data/app/~~SMTpqcyDSaJgg9gIsAL2DA==/com.ovwvwvo.appinfos-K8CDYFIauooNPRorQvLGiA==/oat/arm64/base.odex
 * argv[5]: --input-vdex-fd=9
 * argv[6]: --output-vdex-fd=10
 * argv[7]: --app-image-fd=13
 * argv[8]: --image-format=lz4
 * argv[9]: --profile-file-fd=12
 * argv[10]: --swap-fd=11
 * argv[11]: --classpath-dir=/data/app/~~SMTpqcyDSaJgg9gIsAL2DA==/com.ovwvwvo.appinfos-K8CDYFIauooNPRorQvLGiA==
 * argv[12]: --class-loader-context=PCL[]{PCL[/system/framework/android.test.base.jar]#PCL[/system/framework/org.apache.http.legacy.jar]}
 * argv[13]: --instruction-set=arm64
 * argv[14]: --instruction-set-features=default
 * argv[15]: --instruction-set-variant=generic
 * argv[16]: --compiler-filter=speed-profile
 * argv[17]: --compilation-reason=cmdline
 * argv[18]: --max-image-block-size=524288
 * argv[19]: --resolve-startup-const-strings=true
 * argv[20]: --generate-mini-debug-info
 * argv[21]: --runtime-arg
 * argv[22]: -Xtarget-sdk-version:24
 * argv[23]: --runtime-arg
 * argv[24]: -Xhidden-api-policy:enabled
 * argv[25]: --runtime-arg
 * argv[26]: -Xms64m
 * argv[27]: --runtime-arg
 * argv[28]: -Xmx512m
 * argv[29]: --inline-max-code-units=0: 这个是添加的用来不处理 inline 的
 */

/**
 * environ 的值
 *
 * environ[0]: PATH=/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin
 * environ[1]: ANDROID_BOOTLOGO=1
 * environ[2]: ANDROID_ROOT=/system
 * environ[3]: ANDROID_ASSETS=/system/app
 * environ[4]: ANDROID_DATA=/data
 * environ[5]: ANDROID_STORAGE=/storage
 * environ[6]: ANDROID_ART_ROOT=/apex/com.android.art
 * environ[7]: ANDROID_I18N_ROOT=/apex/com.android.i18n
 * environ[8]: ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
 * environ[9]: EXTERNAL_STORAGE=/sdcard
 * environ[10]: ASEC_MOUNTPOINT=/mnt/asec
 * environ[11]: DOWNLOAD_CACHE=/data/cache
 * environ[12]: BOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:/system/framework/framework.jar:/system/framework/framework-graphics.jar:/system/framework/ext.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.appsearch/javalib/framework-appsearch.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar:/apex/com.android.ipsec/javalib/android.net.ipsec.ike.jar:/apex/com.android.media/javalib/updatable-media.jar:/apex/com.android.mediaprovider/javalib/framework-mediaprovider.jar:/apex/com.android.os.statsd/javalib/framework-statsd.jar:/apex/com.android.permission/javalib/framework-permission.jar:/apex/com.android.permission/javalib/framework-permission-s.jar:/apex/com.andro
 * environ[13]: DEX2OATBOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:/system/framework/framework.jar:/system/framework/framework-graphics.jar:/system/framework/ext.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/apex/com.android.i18n/javalib/core-icu4j.jar
 * environ[14]: SYSTEMSERVERCLASSPATH=/system/framework/com.android.location.provider.jar:/system/framework/services.jar:/system/framework/ethernet-service.jar:/apex/com.android.appsearch/javalib/service-appsearch.jar:/apex/com.android.media/javalib/service-media-s.jar:/apex/com.android.permission/javalib/service-permission.jar
 * environ[15]: ANDROID_LOG_TAGS=*:v
 *
 */
