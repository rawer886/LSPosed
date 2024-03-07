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
 * Copyright (C) 2021 - 2022 LSPosed Contributors
 */

//
// Created by loves on 2/7/2021.
//

#include <dobby.h>
#include <thread>
#include "loader.h"
#include "service.h"
#include "context.h"
#include "utils/jni_helper.hpp"
#include "symbol_cache.h"
#include "config_bridge.h"
#include "elf_util.h"

using namespace lsplant;

namespace lspd {
    std::unique_ptr<Service> Service::instance_ = std::make_unique<Service>();

    jboolean
    Service::exec_transact_replace(jboolean *res, JNIEnv *env, [[maybe_unused]] jobject obj,
                                   va_list args) {
        va_list copy;
        va_copy(copy, args);
        auto code = va_arg(copy, jint);
        auto data_obj = va_arg(copy, jlong);
        auto reply_obj = va_arg(copy, jlong);
        auto flags = va_arg(copy, jint);
        va_end(copy);

        if (code == BRIDGE_TRANSACTION_CODE) [[unlikely]] {
            *res = JNI_CallStaticBooleanMethod(env, instance()->bridge_service_class_,
                                               instance()->exec_transact_replace_methodID_,
                                               obj, code, data_obj, reply_obj, flags);
            return true;
        } else if (SET_ACTIVITY_CONTROLLER_CODE != -1 &&
                   code == SET_ACTIVITY_CONTROLLER_CODE) [[unlikely]] {
            va_copy(copy, args);
            /**
             * 调用方是 app 权限，为什么在这里替换掉之后就能绕过权限检查执行 ActivityManagerService#setActivityController ？
             *
             * 1. 首先权限校验是在调用 setActivityController 的时候校验的，然而我们并没有直接去调用改方法
             * 2. 我们是先去调用 activity_manager_service 的 transact 方法
             * 3. 然后拦截的是 transact 方法的服务端处理，这个是在 Binder#execTransact 中处理的（system_process）
             * 4. 之后就可以在 execTransact 中调用 setActivityController 方法
             */
            if (instance()->replace_activity_controller_methodID_) {
                *res = JNI_CallStaticBooleanMethod(env, instance()->bridge_service_class_,
                                                   instance()->replace_activity_controller_methodID_,
                                                   obj, code, data_obj, reply_obj, flags);
            }
            va_end(copy);
            // fallback the backup
        } else if (code == (('_' << 24) | ('C' << 16) | ('M' << 8) | 'D')) {//1598246212
            va_copy(copy, args);
            if (instance()->replace_shell_command_methodID_) {
                *res = JNI_CallStaticBooleanMethod(env, instance()->bridge_service_class_,
                                                   instance()->replace_shell_command_methodID_,
                                                   obj, code, data_obj, reply_obj, flags);
            }
            va_end(copy);
            return *res;
        }
        return false;
    }

    /**
     * 筛选 jni#CallBooleanMethodV 方法中 methodId 为 execTransact 的调用
     *  T: 先尝试调用 exec_transact_replace 方法
     *      如果其返回值为 false，则调用原方法
     *  F: 调用原方法 —— call_boolean_method_va_backup_
     */
    jboolean
    Service::call_boolean_method_va_replace(JNIEnv *env, jobject obj, jmethodID methodId,
                                            va_list args) {
        if (methodId == instance()->exec_transact_backup_methodID_) [[unlikely]] {
            jboolean res = false;
            if (exec_transact_replace(&res, env, obj, args)) [[unlikely]] return res;
            // else fallback to backup
        }
        return instance()->call_boolean_method_va_backup_(env, obj, methodId, args);
    }

    //初始化：获取常用 java 方法的签名
    void Service::InitService(JNIEnv *env) {
        if (initialized_) [[unlikely]] return;

        // ServiceManager
        if (auto service_manager_class = JNI_FindClass(env, "android/os/ServiceManager")) {
            service_manager_class_ = JNI_NewGlobalRef(env, service_manager_class);
        } else return;
        get_service_method_ = JNI_GetStaticMethodID(env, service_manager_class_, "getService",
                                                    "(Ljava/lang/String;)Landroid/os/IBinder;");
        if (!get_service_method_) return;

        // IBinder
        if (auto ibinder_class = JNI_FindClass(env, "android/os/IBinder")) {
            transact_method_ = JNI_GetMethodID(env, ibinder_class, "transact",
                                               "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z");
        } else return;

        if (auto binder_class = JNI_FindClass(env, "android/os/Binder")) {
            binder_class_ = JNI_NewGlobalRef(env, binder_class);
        } else return;
        binder_ctor_ = JNI_GetMethodID(env, binder_class_, "<init>", "()V");

        // Parcel
        if (auto parcel_class = JNI_FindClass(env, "android/os/Parcel")) {
            parcel_class_ = JNI_NewGlobalRef(env, parcel_class);
        } else return;
        data_size_method_ = JNI_GetMethodID(env, parcel_class_, "dataSize","()I");
        obtain_method_ = JNI_GetStaticMethodID(env, parcel_class_, "obtain",
                                               "()Landroid/os/Parcel;");
        recycleMethod_ = JNI_GetMethodID(env, parcel_class_, "recycle", "()V");
        write_interface_token_method_ = JNI_GetMethodID(env, parcel_class_, "writeInterfaceToken",
                                                        "(Ljava/lang/String;)V");
        write_int_method_ = JNI_GetMethodID(env, parcel_class_, "writeInt", "(I)V");
        write_string_method_ = JNI_GetMethodID(env, parcel_class_, "writeString",
                                               "(Ljava/lang/String;)V");
        write_strong_binder_method_ = JNI_GetMethodID(env, parcel_class_, "writeStrongBinder",
                                                      "(Landroid/os/IBinder;)V");
        read_exception_method_ = JNI_GetMethodID(env, parcel_class_, "readException", "()V");
        read_int_method_ = JNI_GetMethodID(env, parcel_class_, "readInt", "()I");
        read_long_method_ = JNI_GetMethodID(env, parcel_class_, "readLong", "()J");
        read_strong_binder_method_ = JNI_GetMethodID(env, parcel_class_, "readStrongBinder",
                                                     "()Landroid/os/IBinder;");
        read_string_method_ = JNI_GetMethodID(env, parcel_class_, "readString",
                                              "()Ljava/lang/String;");
        read_file_descriptor_method_ = JNI_GetMethodID(env, parcel_class_, "readFileDescriptor",
                                                       "()Landroid/os/ParcelFileDescriptor;");
//        createStringArray_ = env->GetMethodID(parcel_class_, "createStringArray",
//                                              "()[Ljava/lang/String;");

        if (auto parcel_file_descriptor_class = JNI_FindClass(env, "android/os/ParcelFileDescriptor")) {
            parcel_file_descriptor_class_ = JNI_NewGlobalRef(env, parcel_file_descriptor_class);
        } else {
            LOGE("ParcelFileDescriptor not found");
            return;
        }
        detach_fd_method_ = JNI_GetMethodID(env, parcel_file_descriptor_class_, "detachFd", "()I");

        if (auto dead_object_exception_class = JNI_FindClass(env,
                                                             "android/os/DeadObjectException")) {
            deadObjectExceptionClass_ = JNI_NewGlobalRef(env, dead_object_exception_class);
        }
        initialized_ = true;
    }

    std::string GetBridgeServiceName() {
        const auto &obfs_map = ConfigBridge::GetInstance()->obfuscation_map();
        static auto signature = obfs_map.at("org.lsposed.lspd.service.") + "BridgeService";
        return signature;
    }

    /**
     *  1. 获取 BridgeService 内的 execTransact、replaceActivityController、replaceShellCommand 方法
     *  2. 替换 JNIEnv 的函数表(env->functions) 的 CallBooleanMethodV 为 call_boolean_method_va_replace
     */
    void Service::HookBridge(const Context &context, JNIEnv *env) {
        static bool kHooked = false;
        // This should only be ran once, so unlikely
        if (kHooked) [[unlikely]] return;
        //如果 InitService 没有执行结束，直接返回
        if (!initialized_) [[unlikely]] return;
        kHooked = true;
        if (auto bridge_service_class = context.FindClassFromCurrentLoader(env,
                                                                           GetBridgeServiceName()))
            bridge_service_class_ = JNI_NewGlobalRef(env, bridge_service_class);
        else {
            LOGE("server class not found");
            return;
        }

        constexpr const auto *hooker_sig = "(Landroid/os/IBinder;IJJI)Z";

        exec_transact_replace_methodID_ = JNI_GetStaticMethodID(env, bridge_service_class_,
                                                                "execTransact",
                                                                hooker_sig);
        if (!exec_transact_replace_methodID_) {
            LOGE("execTransact class not found");
            return;
        }


        replace_activity_controller_methodID_ = JNI_GetStaticMethodID(env, bridge_service_class_,
                                                                      "replaceActivityController",
                                                                      hooker_sig);
        if (!replace_activity_controller_methodID_) {
            LOGE("replaceActivityShell class not found");
        }

        replace_shell_command_methodID_ = JNI_GetStaticMethodID(env, bridge_service_class_,
                                                                "replaceShellCommand",
                                                                hooker_sig);
        if (!replace_shell_command_methodID_) {
            LOGE("replaceShellCommand class not found");
        }

        auto binder_class = JNI_FindClass(env, "android/os/Binder");
        exec_transact_backup_methodID_ = JNI_GetMethodID(env, binder_class, "execTransact",
                                                         "(IJJI)Z");
        //art::JNIEnvExt::SetTableOverride(JNINativeInterface const*)
        auto *setTableOverride = SandHook::ElfImg("/libart.so").getSymbAddress<void (*)(JNINativeInterface *)>(
                "_ZN3art9JNIEnvExt16SetTableOverrideEPK18JNINativeInterface");
        if (!setTableOverride) {
            LOGE("set table override not found");
        }
        /**
         *  1. 拷贝 JNIEnv 的函数表(env->functions) 到 native_interface_replace_
         *  2. 替换 CallBooleanMethodV 为 call_boolean_method_va_replace, 并保存原方法到 call_boolean_method_va_backup_
         *  3. 更新 JNIEnv 的函数表
         */
        memcpy(&native_interface_replace_, env->functions, sizeof(JNINativeInterface));

        call_boolean_method_va_backup_ = env->functions->CallBooleanMethodV;
        native_interface_replace_.CallBooleanMethodV = &call_boolean_method_va_replace;

        if (setTableOverride != nullptr) {
            setTableOverride(&native_interface_replace_);
        }
        if (auto activity_thread_class = JNI_FindClass(env, "android/app/IActivityManager$Stub")) {
            if (auto *set_activity_controller_field = JNI_GetStaticFieldID(env,
                                                                           activity_thread_class,
                                                                           "TRANSACTION_setActivityController",
                                                                           "I")) {
                SET_ACTIVITY_CONTROLLER_CODE = JNI_GetStaticIntField(env, activity_thread_class,
                                                                     set_activity_controller_field);
            }
        }

        LOGD("Done InitService");
    }

    /**
     * 获取 LSPApplicationService 的 binder
     *
     *  1. 获取 ActivityManagerService 的 binder
     *  2. 调用 ActivityManagerService#transact(BRIDGE_TRANSACTION_CODE, data, reply, 0)
     *  3. 从 reply 中获取 LSPApplicationService binder
     *
     * 为什么调用上面的步骤二，就能获取 LSPApplicationService 的 binder？
     *   因为调用 ActivityManagerService#transact 时，在 binder 内部会触发 binde#execTransact。
     *   因为受 call_boolean_method_va_replace 方法的影响，然后就被转移到了 BridgeService#execTransact 方法
     *
     * 为什么选用 "activity" 这个服务？
     *   因为这个方法是在应用启动的时候被调用的，普通应用没有办法获取 serial 服务，所以只能换一个更常用的服务，就选择了它
    */
    ScopedLocalRef<jobject> Service::RequestBinder(JNIEnv *env, jstring nice_name) {
        if (!initialized_) [[unlikely]] {
            LOGE("Service not initialized");
            return {env, nullptr};
        }

        auto bridge_service_name = JNI_NewStringUTF(env, BRIDGE_SERVICE_NAME.data());
        auto bridge_service = JNI_CallStaticObjectMethod(env, service_manager_class_,
                                                         get_service_method_, bridge_service_name);
        if (!bridge_service) {
            LOGD("can't get {}", BRIDGE_SERVICE_NAME);
            return {env, nullptr};
        }

        auto heart_beat_binder = JNI_NewObject(env, binder_class_, binder_ctor_);

        auto data = JNI_CallStaticObjectMethod(env, parcel_class_, obtain_method_);
        auto reply = JNI_CallStaticObjectMethod(env, parcel_class_, obtain_method_);

        auto descriptor = JNI_NewStringUTF(env, BRIDGE_SERVICE_DESCRIPTOR.data());
        JNI_CallVoidMethod(env, data, write_interface_token_method_, descriptor);
        JNI_CallVoidMethod(env, data, write_int_method_, BRIDGE_ACTION_GET_BINDER);
        JNI_CallVoidMethod(env, data, write_string_method_, nice_name);
        JNI_CallVoidMethod(env, data, write_strong_binder_method_, heart_beat_binder);

        auto res = JNI_CallBooleanMethod(env, bridge_service, transact_method_,
                                         BRIDGE_TRANSACTION_CODE,
                                         data,
                                         reply, 0);

        ScopedLocalRef<jobject> service = {env, nullptr};
        if (res) {
            JNI_CallVoidMethod(env, reply, read_exception_method_);
            service = JNI_CallObjectMethod(env, reply, read_strong_binder_method_);
        }
        JNI_CallVoidMethod(env, data, recycleMethod_);
        JNI_CallVoidMethod(env, reply, recycleMethod_);
        if (service) {
            // 为了保证 heart_beat_binder 不被回收 ？
            JNI_NewGlobalRef(env, heart_beat_binder);
        }

        return service;
    }

    /**
     *  获取 LSPSystemServerService 的 binder, 通过 ServiceManager#getService("serial") 获取
     *
     *  阻塞式获取，最多尝试 3 次，每次间隔 1s
     *  因为 LSPSystemServerService 是在 zygote 进程启动的时候就被启动的，所以一般情况 3s 内一定能获取到
     */
    ScopedLocalRef<jobject> Service::RequestSystemServerBinder(JNIEnv *env) {
        if (!initialized_) [[unlikely]] {
            LOGE("Service not initialized");
            return {env, nullptr};
        }
        // Get Binder for LSPSystemServerService.
        // The binder itself was inject into system service "serial"
        auto bridge_service_name = JNI_NewStringUTF(env, SYSTEM_SERVER_BRIDGE_SERVICE_NAME);
        ScopedLocalRef<jobject> binder{env, nullptr};
        for (int i = 0; i < 3; ++i) {
            binder = JNI_CallStaticObjectMethod(env, service_manager_class_,
                                                get_service_method_, bridge_service_name);
            if (binder) {
                LOGD("Got binder for system server");
                break;
            }
            LOGI("Fail to get binder for system server, try again in 1s");
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1s);
        }
        if (!binder) {
            LOGW("Fail to get binder for system server");
            return {env, nullptr};
        }
        return binder;
    }

    /**
     * 获取 LSPApplicationService 的 binder
     * 1. 调用 LSPSystemServerService#transact(BRIDGE_TRANSACTION_CODE, data, reply, 0)
     * 2. 从 reply 中获取 LSPApplicationService binder
     */
    ScopedLocalRef<jobject> Service::RequestApplicationBinderFromSystemServer(JNIEnv *env, const ScopedLocalRef<jobject> &system_server_binder) {
        auto heart_beat_binder = JNI_NewObject(env, binder_class_, binder_ctor_);
        Wrapper wrapper{env, this};

        JNI_CallVoidMethod(env, wrapper.data, write_int_method_, getuid());
        JNI_CallVoidMethod(env, wrapper.data, write_int_method_, getpid());
        JNI_CallVoidMethod(env, wrapper.data, write_string_method_, JNI_NewStringUTF(env, "system"));
        JNI_CallVoidMethod(env, wrapper.data, write_strong_binder_method_, heart_beat_binder);

        auto res = wrapper.transact(system_server_binder, BRIDGE_TRANSACTION_CODE);

        ScopedLocalRef<jobject> app_binder = {env, nullptr};
        if (res) {
            JNI_CallVoidMethod(env, wrapper.reply, read_exception_method_);
            app_binder = JNI_CallObjectMethod(env, wrapper.reply, read_strong_binder_method_);
        }
        if (app_binder) {
            JNI_NewGlobalRef(env, heart_beat_binder);
        }
        LOGD("app_binder: {}", static_cast<void*>(app_binder.get()));
        return app_binder;
    }

    /**
     *  获取 framework/lspd.dex 文件
     *  1. 调用 LSPApplicationService#transact(DEX_TRANSACTION_CODE, data, reply, 0)
     *  2. 从 reply 中获取文件描述符和文件大小
     *  3. 返回文件描述符和文件大小
     */
    std::tuple<int, size_t> Service::RequestLSPDex(JNIEnv *env, const ScopedLocalRef<jobject> &binder) {
        Wrapper wrapper{env, this};
        bool res = wrapper.transact(binder, DEX_TRANSACTION_CODE);
        if (!res) {
            LOGE("Service::RequestLSPDex: transaction failed?");
            return {-1, 0};
        }
        auto parcel_fd = JNI_CallObjectMethod(env, wrapper.reply, read_file_descriptor_method_);
        int fd = JNI_CallIntMethod(env, parcel_fd, detach_fd_method_);
        auto size = static_cast<size_t>(JNI_CallLongMethod(env, wrapper.reply, read_long_method_));
        LOGD("fd={}, size={}", fd, size);
        return {fd, size};
    }

    /**
     * 获取 obfuscation map，用来混淆 Xposed 的 api 签名，参考：ObfuscationManager.getSignatures()
     *  1. 调用 LSPApplicationService#transact(OBFUSCATION_MAP_TRANSACTION_CODE, data, reply, 0)
     *  2. 判断返回的 reply.size 是否为偶数(因为返回的值是将 key、value 交替写入的)
     *  3. 读取 reply 中的 key、value，写入 map 中
     */
    std::map<std::string, std::string>
    Service::RequestObfuscationMap(JNIEnv *env, const ScopedLocalRef<jobject> &binder) {
        std::map<std::string, std::string> ret;
        Wrapper wrapper{env, this};
        bool res = wrapper.transact(binder, OBFUSCATION_MAP_TRANSACTION_CODE);

        if (!res) {
            LOGE("Service::RequestObfuscationMap: transaction failed?");
            return ret;
        }
        auto size = JNI_CallIntMethod(env, wrapper.reply, read_int_method_);
        if (!size || (size & 1) == 1) {
            LOGW("Service::RequestObfuscationMap: invalid parcel size");
        }

        auto get_string = [this, &wrapper, &env]() -> std::string {
            auto s = JNI_Cast<jstring>(JNI_CallObjectMethod(env, wrapper.reply, read_string_method_));
            return JUTFString(s);
        };
        for (auto i = 0; i < size / 2; i++) {
            // DO NOT TOUCH, or value evaluates before key.
            auto &&key = get_string();
            ret[key] = get_string();
        }
#ifndef NDEBUG
        for (const auto &i: ret) {
            LOGD("{} => {}", i.first, i.second);
        }
#endif

        return ret;
    }
}  // namespace lspd
