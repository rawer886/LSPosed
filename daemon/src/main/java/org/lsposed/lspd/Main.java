package org.lsposed.lspd;

import static org.lsposed.lspd.service.ServiceManager.TAG;

import android.os.Process;
import android.util.Log;

import org.lsposed.lspd.service.ServiceManager;

import java.util.Arrays;

public class Main {

    /**
     * 两个启动入口
     * 1. 通过 post-fs-data.sh 脚本启动. 参数为空
     * 2. 通过 service.sh 脚本启动, 携带参数 --from-service
     */
    public static void main(String[] args) {
        Log.i(TAG, "main called with: args = " + Arrays.toString(args) + ", pid = " + Process.myPid() + "|" + Thread.currentThread().getName());
        ServiceManager.start(args);
    }
}
