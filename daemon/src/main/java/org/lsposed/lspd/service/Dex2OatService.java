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

package org.lsposed.lspd.service;

import static org.lsposed.lspd.ILSPManagerService.DEX2OAT_CRASHED;
import static org.lsposed.lspd.ILSPManagerService.DEX2OAT_MOUNT_FAILED;
import static org.lsposed.lspd.ILSPManagerService.DEX2OAT_OK;
import static org.lsposed.lspd.ILSPManagerService.DEX2OAT_SELINUX_PERMISSIVE;
import static org.lsposed.lspd.ILSPManagerService.DEX2OAT_SEPOLICY_INCORRECT;

import android.net.LocalServerSocket;
import android.os.Build;
import android.os.FileObserver;
import android.os.Process;
import android.os.SELinux;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import androidx.annotation.Nullable;
import androidx.annotation.RequiresApi;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;

@RequiresApi(Build.VERSION_CODES.Q)
public class Dex2OatService implements Runnable {
    private static final String TAG = "LSPosedDex2Oat";
    //这个是相对目录,会自动拼接到当前的工作目录 (/data/adb/modules/zygisk_lsposed/)
    private static final String WRAPPER32 = "bin/dex2oat32";
    private static final String WRAPPER64 = "bin/dex2oat64";

    //dex2oat 文件的 path
    private final String[] dex2oatArray = new String[4];
    //dex2oat 文件的 fd
    private final FileDescriptor[] fdArray = new FileDescriptor[4];
    private final FileObserver selinuxObserver;
    private int compatibility = DEX2OAT_OK;

    private void openDex2oat(int id, String path) {
        try {
            //O_RDONLY: 只读; mode: 0 表示不设置权限
            var fd = Os.open(path, OsConstants.O_RDONLY, 0);
            dex2oatArray[id] = path;
            fdArray[id] = fd;
        } catch (ErrnoException ignored) {
        }
    }

    public Dex2OatService() {
        Log.d(TAG, "urer.dir = " + System.getProperty("user.dir"));

        //这个类默认在 > Android 10 上运行，所以不需要处理 Android 10 以下的情况
        if (Build.VERSION.SDK_INT == Build.VERSION_CODES.Q) {
            openDex2oat(Process.is64Bit() ? 2 : 0, "/apex/com.android.runtime/bin/dex2oat");
            openDex2oat(Process.is64Bit() ? 3 : 1, "/apex/com.android.runtime/bin/dex2oatd");
        } else {
            //32 位没有后两个; 64 位没有前两个
            openDex2oat(0, "/apex/com.android.art/bin/dex2oat32");
            openDex2oat(1, "/apex/com.android.art/bin/dex2oatd32");
            openDex2oat(2, "/apex/com.android.art/bin/dex2oat64");
            openDex2oat(3, "/apex/com.android.art/bin/dex2oatd64");
        }

        //用于控制SELinux的强制模式. 值为 1 时, SELinux 强制执行; 值为 0 时, SELinux 宽容执行; 值为 -1 时, SELinux 不可用
        var enforce = Paths.get("/sys/fs/selinux/enforce");
        //用于存储SELinux策略文件
        var policy = Paths.get("/sys/fs/selinux/policy");
        var list = new ArrayList<File>();
        list.add(enforce.toFile());
        list.add(policy.toFile());
        //监听 SELinux 状态变化, 如果发生变化就检测环境是否符合要求. CLOSE_WRITE: 文件写入并关闭时触发.等同于一次完整的写入
        selinuxObserver = new FileObserver(list, FileObserver.CLOSE_WRITE) {
            @Override
            public synchronized void onEvent(int i, @Nullable String s) {
                Log.d(TAG, String.format("SELinux status changed %d | %s", i, s));
                if (compatibility == DEX2OAT_CRASHED) {
                    stopWatching();
                    return;
                }

                boolean enforcing = false;
                try (var is = Files.newInputStream(enforce)) {
                    enforcing = is.read() == '1';
                } catch (IOException ignored) {
                }

                /**
                 * 1. 如果 SELinux 不可用(不是 "强制执行" 模式):
                 *              那么不需要处理, 则标记为 DEX2OAT_SELINUX_PERMISSIVE 并 unmount dex2oat
                 * 2. 如果 SELinux 是 "强制执行" 模式, 并且 "不信任应用" 能执行 dex2oat file:
                 *              那么标记为 DEX2OAT_SEPOLICY_INCORRECT 并 unmount dex2oat
                 * 3. 如果 SELinux 是 "强制执行" 模式, compatibility 不是 DEX2OAT_OK:
                 *              那么重试挂载,并根据结果重新标记 compatibility
                 */
                if (!enforcing) {
                    if (compatibility == DEX2OAT_OK) doMount(false);
                    compatibility = DEX2OAT_SELINUX_PERMISSIVE;
                } else if (SELinux.checkSELinuxAccess("u:r:untrusted_app:s0", "u:object_r:dex2oat_exec:s0", "file", "execute")
                        || SELinux.checkSELinuxAccess("u:r:untrusted_app:s0", "u:object_r:dex2oat_exec:s0", "file", "execute_no_trans")) {
                    if (compatibility == DEX2OAT_OK) doMount(false);
                    compatibility = DEX2OAT_SEPOLICY_INCORRECT;
                } else if (compatibility != DEX2OAT_OK) {
                    doMount(true);
                    if (notMounted()) {
                        doMount(false);
                        compatibility = DEX2OAT_MOUNT_FAILED;
                        stopWatching();
                    } else {
                        compatibility = DEX2OAT_OK;
                    }
                } else {
                    Log.i(TAG, "SELinux status changed, but no need to do anything");
                }
            }

            @Override
            public void stopWatching() {
                super.stopWatching();
                Log.w(TAG, "SELinux observer stopped");
            }
        };
    }

    private boolean notMounted() {
        for (int i = 0; i < dex2oatArray.length; i++) {
            var bin = dex2oatArray[i];
            if (bin == null) continue;
            try {
                // 目录 /proc/1/root 指向的是 / 的根目录
                var apex = Os.stat("/proc/1/root" + bin);
                var wrapper = Os.stat(i < 2 ? WRAPPER32 : WRAPPER64);
                //st_dev: 文件所在的设备 ID; st_ino: 文件的 inode; 二者可以唯一确定一个文件
                if (apex.st_dev != wrapper.st_dev || apex.st_ino != wrapper.st_ino) {
                    Log.w(TAG, "Check mount failed for " + bin);
                    return true;
                }
            } catch (ErrnoException e) {
                Log.e(TAG, "Check mount failed for " + bin, e);
                return true;
            }
        }
        Log.d(TAG, "Check mount succeeded");
        return false;
    }

    private void doMount(boolean enabled) {
        Log.i(TAG, "doMountNative dex2oatArray: " + Arrays.toString(dex2oatArray));
        doMountNative(enabled, dex2oatArray[0], dex2oatArray[1], dex2oatArray[2], dex2oatArray[3]);
    }

    public void start() {
        if (notMounted()) { // Already mounted when restart daemon
            doMount(true);
            if (notMounted()) {
                doMount(false);
                compatibility = DEX2OAT_MOUNT_FAILED;
                return;
            }
        }

        var thread = new Thread(this);
        thread.setName("dex2oat");
        thread.start();
        selinuxObserver.startWatching();
        selinuxObserver.onEvent(0, null);
        Log.i(TAG, "Dex2oatService started END!");
    }

    @Override
    public void run() {
        Log.i(TAG, "Dex2oatService Thread start");
        var sockPath = getSockPath();
        Log.d(TAG, "wrapper path: " + sockPath);
        var magisk_file = "u:object_r:magisk_file:s0";
        var dex2oat_exec = "u:object_r:dex2oat_exec:s0";
        /**
         * 检测 u:r:dex2oat:s0 是否有执行 u:r:dex2oat:s0 文件的权限
         *      true:   那么就把 WRAPPER32\WRAPPER64 的上下文设置为 dex2oat_exec
         *      false:  那么就把 WRAPPER32\WRAPPER64 的上下文设置为 magisk_file
         */
        if (SELinux.checkSELinuxAccess("u:r:dex2oat:s0", dex2oat_exec,
                "file", "execute_no_trans")) {
            SELinux.setFileContext(WRAPPER32, dex2oat_exec);
            SELinux.setFileContext(WRAPPER64, dex2oat_exec);
            setSockCreateContext("u:r:dex2oat:s0");
        } else {
            SELinux.setFileContext(WRAPPER32, magisk_file);
            SELinux.setFileContext(WRAPPER64, magisk_file);
            setSockCreateContext("u:r:installd:s0");
        }
        try (var server = new LocalServerSocket(sockPath)) {
            setSockCreateContext(null);
            while (true) {
                Log.i(TAG, "Dex2oatService socket is waiting...");
                try (var client = server.accept();
                     var is = client.getInputStream();
                     var os = client.getOutputStream()) {
                    var id = is.read();
                    var fd = new FileDescriptor[]{fdArray[id]};
                    client.setFileDescriptorsForSend(fd);
                    os.write(1);
                    Log.d(TAG, "Sent stock fd: is64 = " + ((id & 0b10) != 0) + ", isDebug = " + ((id & 0b01) != 0));
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "Dex2oat wrapper daemon crashed", e);
            if (compatibility == DEX2OAT_OK) {
                doMount(false);
                compatibility = DEX2OAT_CRASHED;
            }
        }
    }

    public int getCompatibility() {
        return compatibility;
    }

    private native void doMountNative(boolean enabled,
                                      String r32, String d32, String r64, String d64);

    private static native boolean setSockCreateContext(String context);

    private native String getSockPath();
}
