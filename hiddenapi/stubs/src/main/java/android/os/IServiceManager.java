package android.os;

public interface IServiceManager extends IInterface {

    void tryUnregisterService(java.lang.String name, android.os.IBinder service);

    IBinder getService(String name);

    /**
     * Request a callback when a service is registered.
     */
    public void registerForNotifications(String name, IServiceCallback cb);

    abstract class Stub extends Binder implements IServiceManager {
        public static IServiceManager asInterface(IBinder obj) {
            throw new UnsupportedOperationException();
        }
    }
}
