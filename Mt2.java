

package com.mt2;

public class Mt2 {
    static {
        System.loadLibrary("mt2");
    }


    public static native String version();


    public static native boolean hasZipSupport();


    public static native int exec(String cmd, String[] args);


    public static native int execStr(String cmdStr);


    public static native void setLanguage(int lang);


    public static native String help();


    public static boolean copy(String src, String dst) {
        return exec("copy", new String[]{src, dst}) == 0;
    }

    public static boolean move(String src, String dst) {
        return exec("move", new String[]{src, dst}) == 0;
    }

    public static boolean delete(String path) {
        return exec("delete", new String[]{path}) == 0;
    }

    public static boolean deleteForce(String path) {
        return exec("delete", new String[]{path, "--force"}) == 0;
    }

    public static String[] list(String dir) {


        exec("list", new String[]{dir});
        return null;
    }

    public static boolean write(String path, String content) {
        return exec("write", new String[]{path, content}) == 0;
    }

    public static boolean chmod(String path, String mode) {
        return exec("chmod", new String[]{path, mode}) == 0;
    }

    public static boolean replace(String file, String search, String replace) {
        return exec("replace", new String[]{file, search, replace}) == 0;
    }
}
