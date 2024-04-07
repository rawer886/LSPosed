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
// Created by Kotori2 on 2021/12/1.
//

#include <jni.h>
#include <unistd.h>
#include <algorithm>
#include <random>
#include <unordered_map>
#include <sys/mman.h>
#include <android/sharedmem.h>
#include <android/sharedmem_jni.h>
#include <slicer/dex_utf8.h>
#include <fcntl.h>
#include "slicer/reader.h"
#include "slicer/writer.h"
#include "obfuscation.h"
#include "logging.h"

using namespace lsplant;
namespace {
std::mutex init_lock{};
std::map<const std::string, std::string> signatures = {
        {"Lde/robv/android/xposed/", ""},
        { "Landroid/app/AndroidApp", ""},
        { "Landroid/content/res/XRes", ""},
        { "Landroid/content/res/XModule", ""},
        { "Lorg/lsposed/lspd/core/", ""},
        { "Lorg/lsposed/lspd/nativebridge/", ""},
        { "Lorg/lsposed/lspd/service/", ""},
};

jclass class_file_descriptor;
jmethodID method_file_descriptor_ctor;

jclass class_shared_memory;
jmethodID method_shared_memory_ctor;

bool inited = false;
}

// 将原始签名转换为 Java 签名
static std::string to_java(const std::string &signature) {
    std::string java(signature, 1);
    replace(java.begin(), java.end(), '/', '.');
    return java;
}

/**
 * 初始化 JNI 类和方法，并生成一份 signatures
 */
void maybeInit(JNIEnv *env) {
    if (inited) [[likely]] return;
    // 如果 init_lock 已经被锁定，则阻塞等待; init_lock 离开作用域时自动解锁
    std::lock_guard l(init_lock);
    LOGD("ObfuscationManager.init");
    if (auto file_descriptor = JNI_FindClass(env, "java/io/FileDescriptor")) {
        class_file_descriptor = JNI_NewGlobalRef(env, file_descriptor);
    } else return;

    method_file_descriptor_ctor = JNI_GetMethodID(env, class_file_descriptor, "<init>", "(I)V");

    if (auto shared_memory = JNI_FindClass(env, "android/os/SharedMemory")) {
        class_shared_memory = JNI_NewGlobalRef(env, shared_memory);
    } else return;

    method_shared_memory_ctor = JNI_GetMethodID(env, class_shared_memory, "<init>", "(Ljava/io/FileDescriptor;)V");

    auto regen = [](std::string_view original_signature) {
        static auto& chrs = "abcdefghijklmnopqrstuvwxyz"
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

        /**
         * 定义几个 uniform_int_distribution 用来生成随机字符串。其中 rg 用来当做随机数生成器的引擎
         *  使用 thread_local 保证每个线程都有自己的随机数生成器，用于保证多线程安全(貌似这里用不到)
         *  std::mt19937: 一种基于梅森旋转算法的伪随机数生成器
         *  std::uniform_int_distribution: 用于生成在指定范围内均匀分布的随机整数。构造函数需要两个参数，分别是随机数的最小值和最大值
         *  std::string::size_type：表示无符号整数类型。在这里用来约束生成的随机数的类型
         *
         *  pick: 用于生成随机的下标。 返回在 0 - sizeof(chrs)-2 之间的随机数，即返回一个 chrs 长度范围内随机的下标
         *      为什么要 -2 呢？因为 sizeof(chrs) 是包含了字符串的结束符的，所以要 - 2
         *
         *  choose_slash: 用于生成随机的斜杠, 返回在 0 - 10 之间的随机数，即返回一个随机的下标
         *      为什么要 10 呢？因为要 80% 的概率是字母，20% 的概率是斜杠。下面用 > 8 来判断
         *
         *  为什么不直接用 std::random_device{}() 生成随机数呢，而是用 std::mt19937 呢？
         *      首先虽然 std::random_device{}() 生成的随机数是真随机数，而 std::mt19937 生成的是伪随机数。
         *      但是 std::random_device{}() 生成的随机数是很慢的，但 std::mt19937 生成的是很快的；
         *      而且 std::mt19937 生成的随机数也是有限的
         *
         *     需要注意，每次初始化 mt19937 都需要使用不同的种子，否则生成的随机数序列是一样的
         */
        thread_local static std::mt19937 rg{std::random_device{}()};
        thread_local static std::uniform_int_distribution<std::string::size_type> pick(0, sizeof(chrs) - 2);
        thread_local static std::uniform_int_distribution<std::string::size_type> choose_slash(0, 10);

        std::string out;
        size_t length = original_signature.size();
        out.reserve(length);//预分配内存
        out += "L";//签名的第一个字符是 L

        for (size_t i = 1; i < length - 1; i++) {
            if (choose_slash(rg) > 8 &&                         // 80% alphabet + 20% slashes
                out[i - 1] != '/' &&                                // slashes could not stick together
                i != 1 &&                                           // the first character should not be slash
                i != length - 2) {                                  // and the last character
                out += "/";
            } else {
                out += chrs[pick(rg)];
            }
        }

        out += "/";//签名的最后一个字符是 /
        return out;
    };

    for (auto &i: signatures) {
        i.second = regen(i.first);
        LOGD("%s => %s", i.first.c_str(), i.second.c_str());
    }

    LOGD("ObfuscationManager init successfully");
    inited = true;
}

// https://stackoverflow.com/questions/4844022/jni-create-hashmap with modifications
jobject stringMapToJavaHashMap(JNIEnv *env, const decltype(signatures)& map) {
    jclass mapClass = env->FindClass("java/util/HashMap");
    if(mapClass == nullptr)
        return nullptr;

    jmethodID init = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, init);
    jmethodID put = env->GetMethodID(mapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    auto citr = map.begin();
    for( ; citr != map.end(); ++citr) {
        jstring keyJava = env->NewStringUTF(citr->first.c_str());
        jstring valueJava = env->NewStringUTF(citr->second.c_str());

        env->CallObjectMethod(hashMap, put, keyJava, valueJava);

        env->DeleteLocalRef(keyJava);
        env->DeleteLocalRef(valueJava);
    }

    auto hashMapGobal = static_cast<jobject>(env->NewGlobalRef(hashMap));
    env->DeleteLocalRef(hashMap);
    env->DeleteLocalRef(mapClass);

    return hashMapGobal;
}

extern "C"
JNIEXPORT jobject JNICALL
Java_org_lsposed_lspd_service_ObfuscationManager_getSignatures(JNIEnv *env, [[maybe_unused]] jclass obfuscation_manager) {
    maybeInit(env);
    static jobject signatures_jni = nullptr;
    // signatures_jni 是静态的。如果已经初始化过了，就直接返回
    if (signatures_jni) return signatures_jni;
    decltype(signatures) signatures_java;
    for (const auto &i: signatures) {
        signatures_java[to_java(i.first)] = to_java(i.second);
    }
    signatures_jni = stringMapToJavaHashMap(env, signatures_java);
    return signatures_jni;
}

/**
 * 遍历 dex 文件中的字符串，将其中的签名进行混淆处理
 * 1. 从 dex 文件中读取字符串
 * 2. 遍历字符串，如果字符串中包含 signatures 中的签名，则替换为混淆后的字符串
 * 3. 将混淆后的 dex 文件写入到新的内存中，避免对原内存进行修改
 * 4. 返回新的内存的 fd
 */
static int obfuscateDex(const void *dex, size_t size) {
    // const char* new_sig = obfuscated_signature.c_str();
    dex::Reader reader{reinterpret_cast<const dex::u1*>(dex), size};

    reader.CreateFullIr();
    auto ir = reader.GetIr();
    for (auto &i: ir->strings) {
        const char *s = i->c_str();
        for (const auto &signature: signatures) {
            char* p = const_cast<char *>(strstr(s, signature.first.c_str()));
            if (p) {
                auto new_sig = signature.second.c_str();
                // NOLINTNEXTLINE bugprone-not-null-terminated-result
                memcpy(p, new_sig, strlen(new_sig));
            }
        }
    }
    /**
     * 1. 为什么不用 memory 已经修改过的 dex 呢？
     *      因为 memory 被修改的只是 dex 里面的 str 字符串, 如果想要使用修改后的 dex 就需要重新打包（比如 checksum 就需要在打包的时候重新生成）
     *     这里只能使用 dex::Writer 的 api 进行重新打包
     *
     * 2. 重新打包的 dex 什么一定要写入到共享内存中吗？ todo
    */
    //上面已经完成对内存中 dex 文件的混淆，但是为了能够获取一个混淆后 dex 文件的 fd，需要将混淆后的 dex 文件写入到共享内存
    dex::Writer writer(ir);

    size_t new_size;
    WA allocator;
    auto *p_dex = writer.CreateImage(&allocator, &new_size);  // allocates memory only once
    return allocator.GetFd(p_dex);
}

/**
 * 从共享内存中获取 dex 文件，然后进行混淆
 *  1. 从共享内存中获取 fd
 *  2. 使用 mmap 将共享内存映射到内存中
 *  3. 调用 obfuscateDex 进行混淆
 *  4. 构造新的共享内存, 并返回
 */
extern "C"
JNIEXPORT jobject
Java_org_lsposed_lspd_service_ObfuscationManager_obfuscateDex(JNIEnv *env, [[maybe_unused]] jclass obfuscation_manager,
                                                              jobject memory) {
    maybeInit(env);
    int fd = ASharedMemory_dupFromJava(env, memory);
    auto size = ASharedMemory_getSize(fd);
    LOGD("fd=%d, size=%zu", fd, size);

    const void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        LOGE("old dex map failed?");
        return nullptr;
    }

    auto new_fd = obfuscateDex(mem, size);

    // 使用新的 fd 构造一个新的共享内存，并返回到 Java 层 ——  这里创建共享内存最简单的方法就是使用带 fd 的构造函数
    auto java_fd = JNI_NewObject(env, class_file_descriptor, method_file_descriptor_ctor, new_fd);
    auto java_sm = JNI_NewObject(env, class_shared_memory, method_shared_memory_ctor, java_fd);

    return java_sm.release();
}
