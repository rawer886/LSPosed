package android.os;

public class SELinux {
    /**
     * Check permissions between two security contexts.
     *
     * @param scon   The source or subject security context.
     * @param tcon   The target or object security context.
     * @param tclass The object security class name.
     * @param perm   The permission name.
     * @return a boolean indicating whether permission was granted.
     * <p>
     * <p>
     * 举个例子: SELinux.checkSELinuxAccess("u:r:untrusted_app:s0", "u:object_r:dex2oat_exec:s0", "file", "execute")
     * 表示一个不受信任的应用(表示一个不被信任的应用) 是否可以执行(execute) dex2oat_exec(u:object_r:dex2oat_exec:s0) 类型的文件 (file)
     */
    public static boolean checkSELinuxAccess(String scon, String tcon, String tclass, String perm) {
        throw new UnsupportedOperationException("Stub");
    }

    public static boolean setFileContext(String path, String context) {
        throw new UnsupportedOperationException("Stub");
    }

    public static boolean setFSCreateContext(String context) {
        throw new UnsupportedOperationException("Stub");
    }
}
