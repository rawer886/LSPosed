package org.lsposed.lspd.service;

import android.annotation.SuppressLint;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SELinux;
import android.os.SystemProperties;
import android.system.Os;
import android.util.Log;

import org.lsposed.daemon.BuildConfig;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.LinkedHashMap;

public class LogcatService implements Runnable {
    private static final String TAG = "LSPosedLogcat";
    // 覆盖写入,不存在则创建,存在则清空; MODE_APPEND 和 MODE_TRUNCATE 互斥了 !!!
    private static final int mode = ParcelFileDescriptor.MODE_WRITE_ONLY |
            ParcelFileDescriptor.MODE_CREATE |
            ParcelFileDescriptor.MODE_TRUNCATE |
            ParcelFileDescriptor.MODE_APPEND;
    private int modulesFd = -1;
    private int verboseFd = -1;
    private Thread thread = null;

    static class LogLRU extends LinkedHashMap<File, Object> {
        private static final int MAX_ENTRIES = 10;

        public LogLRU() {
            super(MAX_ENTRIES, 1f, false);
        }

        @Override
        synchronized protected boolean removeEldestEntry(Entry<File, Object> eldest) {
            if (size() > MAX_ENTRIES && eldest.getKey().delete()) {
                Log.d(TAG, "Deleted old log " + eldest.getKey().getAbsolutePath());
                return true;
            }
            return false;
        }
    }

    @SuppressWarnings("MismatchedQueryAndUpdateOfCollection")
    private final LinkedHashMap<File, Object> moduleLogs = new LogLRU();// 记录日志文件,最多10个
    @SuppressWarnings("MismatchedQueryAndUpdateOfCollection")
    private final LinkedHashMap<File, Object> verboseLogs = new LogLRU();

    @SuppressLint("UnsafeDynamicallyLoadedCode")
    public LogcatService() {
        String classPath = System.getProperty("java.class.path");
        var abi = Process.is64Bit() ? Build.SUPPORTED_64_BIT_ABIS[0] : Build.SUPPORTED_32_BIT_ABIS[0];
        var libPath = classPath + "!/lib/" + abi + "/" + System.mapLibraryName("daemon");
        Log.i(TAG, "Loading library: " + libPath);// /data/adb/modules/zygisk_lsposed/daemon.apk!/lib/arm64-v8a/libdaemon.so
        System.load(libPath);
        ConfigFileManager.moveLogDir();

        // Meizu devices set this prop and prevent debug logs from being recorded
        if (SystemProperties.getInt("persist.sys.log_reject_level", 0) > 0) {
            SystemProperties.set("persist.sys.log_reject_level", "0");
        }

        getprop();
        dmesg();
    }

    // u:r:untrusted_app:s0
    // u:object_r:app_data_file:s0
    private static void getprop() {
        // multithreaded process can not change their context type,
        // start a new process to set restricted context to filter privacy props
        var cmd = "echo -n u:r:untrusted_app:s0 > /proc/thread-self/attr/current; getprop";
        try {
            // 设置文件系统上下文. 之后创建的文件都会使用这个上下文
            SELinux.setFSCreateContext("u:object_r:app_data_file:s0");
            new ProcessBuilder("sh", "-c", cmd)
                    .redirectOutput(ConfigFileManager.getPropsPath())
                    .start();
        } catch (IOException e) {
            Log.e(TAG, "getprop: ", e);
        } finally {
            SELinux.setFSCreateContext(null);
        }
    }

    //内核日志
    private static void dmesg() {
        try {
            new ProcessBuilder("dmesg")
                    .redirectOutput(ConfigFileManager.getKmsgPath())
                    .start();
        } catch (IOException e) {
            Log.e(TAG, "dmesg: ", e);
        }
    }

    private native void runLogcat();

    @Override
    public void run() {
        Log.i(TAG, getClass().getSimpleName() + " start running");
        runLogcat();
        Log.i(TAG, "stopped");
    }

    /**
     * @param isVerboseLog true: 详细日志文件; false: 模块日志文件
     * @return 文件描述符
     */
    @SuppressWarnings("unused")
    private int refreshFd(boolean isVerboseLog) {
        try {
            File log;
            if (isVerboseLog) {
                checkFd(verboseFd);
                log = ConfigFileManager.getNewVerboseLogPath();
            } else {
                checkFd(modulesFd);
                log = ConfigFileManager.getNewModulesLogPath();
            }
            Log.i(TAG, "New log file: " + log);
            ConfigFileManager.chattr0(log.toPath().getParent());
            int fd = ParcelFileDescriptor.open(log, mode).detachFd();
            if (isVerboseLog) {
                synchronized (verboseLogs) {
                    verboseLogs.put(log, new Object());
                }
                verboseFd = fd;
            } else {
                synchronized (moduleLogs) {
                    moduleLogs.put(log, new Object());
                }
                modulesFd = fd;
            }
            return fd;
        } catch (IOException e) {
            if (isVerboseLog)
                verboseFd = -1;
            else
                modulesFd = -1;
            Log.w(TAG, "refreshFd", e);
            return -1;
        }
    }

    /**
     * 检查文件描述符是否有效, 如果被删除就尝试恢复
     */
    private static void checkFd(int fd) {
        if (fd == -1)
            return;
        try {
            var jfd = new FileDescriptor();
            // noinspection JavaReflectionMemberAccess DiscouragedPrivateApi
            jfd.getClass().getDeclaredMethod("setInt$", int.class).invoke(jfd, fd);// 设置文件描述符
            var stat = Os.fstat(jfd);// 获取文件状态
            Log.i(TAG, "checkFd stat.st_nlink = " + stat.st_nlink + ", link = " + fdToPath(fd) + ", path = " + Files.readSymbolicLink(fdToPath(fd)));
            if (stat.st_nlink == 0) {// 硬链接数为0,说明文件已经被删除了,但是文件描述符还有效
                var file = Files.readSymbolicLink(fdToPath(fd));// 根据软连接获取文件真实路径
                var parent = file.getParent();
                // 判断 parent 是否是目录,如果不是目录,则删除 parent. 貌似 parent 一定是目录?
                if (!Files.isDirectory(parent, LinkOption.NOFOLLOW_LINKS)) {
                    if (ConfigFileManager.chattr0(parent))
                        Files.deleteIfExists(parent);
                }
                var name = file.getFileName().toString();
                var originName = name.substring(0, name.lastIndexOf(' '));
                //如果文件被删除, 通过文件描述符感觉也找不到文件. 不太清楚为何这么做?
                Log.i(TAG, String.format("Recovered log file from %s to %s", file, parent.resolve(originName)));
                Files.copy(file, parent.resolve(originName));
            }
        } catch (Throwable e) {
            Log.w(TAG, "checkFd " + fd, e);
        }
    }

    public boolean isRunning() {
        return thread != null && thread.isAlive();
    }

    public void start() {
        if (isRunning())
            return;
        thread = new Thread(this);
        thread.setName("logcat");
        thread.setUncaughtExceptionHandler((t, e) -> {
            Log.e(TAG, "Crash unexpectedly: ", e);
            thread = null;
            start();
        });
        thread.start();
    }

    public void startVerbose() {
        Log.i(TAG, "!!start_verbose!!");
    }

    public void stopVerbose() {
        Log.i(TAG, "!!stop_verbose!!");
    }

    public void refresh(boolean isVerboseLog) {
        if (isVerboseLog) {
            Log.i(TAG, "!!refresh_verbose!!");
        } else {
            Log.i(TAG, "!!refresh_modules!!");
        }
    }

    /**
     * 根据 fd 拼接出文件路径
     *
     * @return /proc/self/fd/[fd]
     */
    private static Path fdToPath(int fd) {
        if (fd == -1)
            return null;
        else
            return Paths.get("/proc/self/fd", String.valueOf(fd));
    }

    public File getVerboseLog() {
        var path = fdToPath(verboseFd);
        return path == null ? null : path.toFile();
    }

    public File getModulesLog() {
        var path = fdToPath(modulesFd);
        return path == null ? null : path.toFile();
    }

    public void checkLogFile() {
        if (modulesFd == -1)
            refresh(false);
        if (verboseFd == -1)
            refresh(true);
    }
}
