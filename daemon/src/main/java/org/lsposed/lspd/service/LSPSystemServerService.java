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

package org.lsposed.lspd.service;

import static org.lsposed.lspd.service.ServiceManager.TAG;
import static org.lsposed.lspd.service.ServiceManager.getSystemServiceManager;

import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.IServiceCallback;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemProperties;
import android.util.Log;

public class LSPSystemServerService extends ILSPSystemServerService.Stub implements IBinder.DeathRecipient {

    /**
     * 这个是系统中用于串口通信的服务 (SerialManager), 通过代理这个服务, 可以实现对系统中串口通信的拦截
     * <p>
     * lsp 中在 Service::RequestSystemServerBinder 中会调用这个服务
     *
     * @see android.content.Context.SERIAL_SERVICE
     * @see android.hardware.SerialManager
     * <p>
     * 选用这个服务的原因可能是因为这个服务不常用
     * 注意: 这个服务名在 Android 9 之前是没有的
     */
    public static final String PROXY_SERVICE_NAME = "serial";

    private IBinder originService = null;
    private int requested;

    public boolean systemServerRequested() {
        return requested > 0;
    }

    public void putBinderForSystemServer() {
        //抢先注册 serial 服务, 这样就可以让系统中的 SerialManager 服务调用到这个服务
        android.os.ServiceManager.addService(PROXY_SERVICE_NAME, this);
        //清理 originService 的死亡通知. 因为这个时候 originService 应该还是 null, 没有被赋值. 除非是重启 lspd 服务的时候 originService 不为 null
        binderDied();
    }

    public LSPSystemServerService(int maxRetry) {
        Log.d(TAG, "new LSPSystemServerService::LSPSystemServerService");
        requested = -maxRetry;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {// Android 11
            // Registers a callback when system is registering an authentic "serial" service
            // And we are proxying all requests to that system service
            var serviceCallback = new IServiceCallback.Stub() {
                @Override
                public void onRegistration(String name, IBinder binder) {
                    //向 ServiceManager 添加服务的时候会调用. putBinderForSystemServer 的时候也会调用, 所以需要添加一个判断
                    Log.d(TAG, "LSPSystemServerService::LSPSystemServerService onRegistration: " + name + " " + binder + " | callPid = " + Binder.getCallingPid());
                    if (name.equals(PROXY_SERVICE_NAME) && binder != null && binder != LSPSystemServerService.this) {
                        Log.d(TAG, "Register " + name + " " + binder);
                        originService = binder;
                        LSPSystemServerService.this.linkToDeath();
                    }
                }

                @Override
                public IBinder asBinder() {//registerForNotifications 的时候会调用
                    return this;
                }
            };
            try {
                getSystemServiceManager().registerForNotifications(PROXY_SERVICE_NAME, serviceCallback);
            } catch (Throwable e) {
                Log.e(TAG, "unregister: ", e);
            }
        }
    }

    @Override
    public ILSPApplicationService requestApplicationService(int uid, int pid, String processName, IBinder heartBeat) {
        Log.d(TAG, "ILSPApplicationService.requestApplicationService: " + uid + " " + pid + " " + processName + " " + heartBeat);
        requested = 1;
        if (ConfigManager.getInstance().shouldSkipSystemServer() || uid != 1000 || heartBeat == null || !"system".equals(processName))
            return null;
        else
            return ServiceManager.requestApplicationService(uid, pid, processName, heartBeat);
    }

    @Override
    public boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        Log.d(TAG, "LSPSystemServerService.onTransact: code=" + code + " | callPid = " + Binder.getCallingPid());
        if (originService != null) {
            Log.d(TAG, "LSPSystemServerService.onTransact: call originService=" + originService);
            return originService.transact(code, data, reply, flags);
        }

        switch (code) {
            case BridgeService.TRANSACTION_CODE -> {//1598837584
                int uid = data.readInt();
                int pid = data.readInt();
                String processName = data.readString();
                IBinder heartBeat = data.readStrongBinder();
                var service = requestApplicationService(uid, pid, processName, heartBeat);
                if (service != null) {
                    Log.d(TAG, "LSPSystemServerService.onTransact requestApplicationService granted: " + service);
                    reply.writeNoException();
                    reply.writeStrongBinder(service.asBinder());
                    return true;
                } else {
                    Log.d(TAG, "LSPSystemServerService.onTransact requestApplicationService rejected");
                    return false;
                }
            }
            case LSPApplicationService.OBFUSCATION_MAP_TRANSACTION_CODE, LSPApplicationService.DEX_TRANSACTION_CODE -> {//724533732
                // Proxy LSP dex transaction to Application Binder
                return ServiceManager.getApplicationService().onTransact(code, data, reply, flags);
            }
            default -> {
                return super.onTransact(code, data, reply, flags);
            }
        }
    }

    public void linkToDeath() {
        try {
            //flag = 0, 表示不会调用 onBinderDied
            originService.linkToDeath(this, 0);
        } catch (Throwable e) {
            Log.e(TAG, "system server service: link to death", e);
        }
    }

    @Override
    public void binderDied() {
        if (originService != null) {
            originService.unlinkToDeath(this, 0);
            originService = null;
        }
    }

    //重启 Zygote
    public void maybeRetryInject() {
        if (requested < 0) {
            Log.w(TAG, "System server injection fails, trying a restart");
            ++requested;
            if (Build.SUPPORTED_64_BIT_ABIS.length > 0 && Build.SUPPORTED_32_BIT_ABIS.length > 0) {
                // Only devices with both 32-bit and 64-bit support have zygote_secondary
                SystemProperties.set("ctl.restart", "zygote_secondary");
            } else {
                SystemProperties.set("ctl.restart", "zygote");
            }
        }
    }
}
