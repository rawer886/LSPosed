package com.android.internal.os;

import android.os.IBinder;

public class BinderInternal {
    /**
     * Return the global "context object" of the system.  This is usually
     * an implementation of IServiceManager, which you can use to find
     * other services.
     */
    public static final native IBinder getContextObject();
}
