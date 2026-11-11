// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge;

/**
 * Java side of the bundled 32-bit ARM demo library (core/guest/guestdemo.c).
 * All native methods below are implemented by ARMv7 machine code that runs
 * through the bridge.
 */
public final class GuestDemo {
    public int counter;

    public static native int add(int a, int b);
    public static native long mulLong(long a, long b);
    public static native double hypot(double a, double b);
    public static native float halve(float v);
    public static native double mixed(int a, long b, float c, double d, byte e, short f, char g, boolean h, long i,
                                      int j, double k, float l, int m, int n, int o, int p, double q, double r,
                                      double s, double t, double u, double v);
    public static native int divide(int a, int b);
    public static native String greet(String name);
    public static native int sumArray(int[] values);
    public native String callbacks(int v);
    public static native void throwIt(String message);
    public static native long fib(int n);
    public static native int primes(int limit);
    public static native int threads(int threads, int rounds);
    public static native int libc();
    public static native int posix();
    public static native void remember(String s);
    public static native int recallLength();
    public static native long frameLimiter(long frameMicros);
    public static native int frameSleep(int elapsedMillis);
    // Registered from JNI_OnLoad with RegisterNatives:
    public native String registered(int x, String s);
    public static native boolean sameEnv();

    // Called from native code.
    // Re-enters the guest library while it is in the middle of a JNI call (nested emulation).
    public static int twice(int v) { return add(v, v); }
    public static String describe(long a, double b, String c, float d) { return a + "|" + b + "|" + c + "|" + d; }
    public int bump(int by) { counter += by; return counter; }

    /** Runs every check and returns a human readable report. The first line is "PASS" or "FAIL". */
    public static String runAll() {
        StringBuilder sb = new StringBuilder();
        int[] failed = {0};
        check(sb, failed, "add", add(40, 2) == 42);
        check(sb, failed, "mulLong", mulLong(123456789L, 1000003L) == 123456789L * 1000003L);
        check(sb, failed, "hypot", hypot(3, 4) == 5.0);
        check(sb, failed, "halve", halve(5f) == 2.5f);
        double mixed = mixed(1, 20000000000L, 1.5f, 2.25, (byte) -3, (short) -300, 'A', true, -5L, 6, 7.5, 0.5f, 1, 2,
                3, 4, 0.125, 1, 2, 3, 4, 5);
        double expect = 1 + 20000000000.0 + 1.5 + 2.25 - 3 - 300 + 65 + 1000 - 5 + 6 + 7.5 + 0.5 + 1 + 2 + 3 + 4 + 0.125
                + 1 + 2 + 3 + 4 + 5;
        check(sb, failed, "mixed arguments (" + mixed + ")", mixed == expect);
        check(sb, failed, "divide", divide(47, 5) == 11 && divide(-47, 5) == -11);
        String g = greet("Android");
        check(sb, failed, "greet -> " + g,
                "Hello Android from 32-bit ARM code! pi=3.1416 big=1234567890123 hex=beef".equals(g));
        int[] arr = {1, 2, 3, 4};
        check(sb, failed, "sumArray", sumArray(arr) == 10 && arr[0] == 2 && arr[3] == 8);
        // FPS booster off: the limiter code must see the game's own frame time.
        check(sb, failed, "frame limiter (booster off)", frameLimiter(33333L) == 33333L);
        // Game loop frame sleep: 10 ms into a 30 ms frame it must sleep about 20 ms (booster off).
        long sleepStart = System.nanoTime();
        int slept = frameSleep(10);
        long sleptMs = (System.nanoTime() - sleepStart) / 1000000;
        check(sb, failed, "frame sleep (booster off, " + sleptMs + " ms)", slept == 1 && sleptMs >= 15 && sleptMs < 200);
        GuestDemo d = new GuestDemo();
        String cb = d.callbacks(21);
        check(sb, failed, "callbacks -> " + cb + " counter=" + d.counter,
                "4294967298|2.5|tag|1.5".equals(cb) && d.counter == 42 + 5 + 100);
        boolean thrown = false;
        try {
            throwIt("boom");
        } catch (IllegalStateException e) {
            thrown = "boom".equals(e.getMessage());
        }
        check(sb, failed, "exception from native code", thrown);
        check(sb, failed, "fib", fib(90) == 2880067194370816120L);
        check(sb, failed, "libc score " + libc(), libc() == 63);
        remember(new String("kept across calls"));
        for (int i = 0; i < 20000; i++) add(i, 2);  // many later calls must not push the reference out
        System.gc();
        check(sb, failed, "object argument used after the call returned", recallLength() == 17);
        int posix = posix();
        check(sb, failed, "zlib / epoll / div_t / wide strings score " + posix, posix == 63);
        long tt = System.nanoTime();
        int th = threads(4, 50000);
        check(sb, failed, "4 parallel threads, atomics + spin lock + mutex (" + (System.nanoTime() - tt) / 1000000 + " ms)", th == 200000);
        check(sb, failed, "RegisterNatives", "registered:14:x".equals(new GuestDemo().registered(7, "x")));
        check(sb, failed, "JavaVM.GetEnv", sameEnv());
        long t0 = System.nanoTime();
        int primes = primes(200000);
        long ms = (System.nanoTime() - t0) / 1000000;
        check(sb, failed, "primes below 200000 = " + primes + " (" + ms + " ms emulated)", primes == 17984);
        return (failed[0] == 0 ? "PASS" : "FAIL (" + failed[0] + ")") + "\n" + sb;
    }

    private static void check(StringBuilder sb, int[] failed, String what, boolean ok) {
        if (!ok) failed[0]++;
        sb.append(ok ? "  ok   " : "  FAIL ").append(what).append('\n');
    }

    private GuestDemo() {}
}
