// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Loads the bridge runtime as soon as a patched app starts, before the app's own classes run. Not
 * used by the ARM64Bridge app itself: the patcher adds this class to apps that run on the bridge and
 * declares it as a content provider, because providers are created when the app starts.
 *
 * Some apps check the CPU from Java before they load their native libraries (the Unity 3 player
 * reads /proc/cpuinfo in a static initializer). The runtime shows such Java code the /proc/cpuinfo
 * of a 32-bit ARM CPU, and it has to be loaded by then.
 */
public final class BridgeStarter extends ContentProvider {
    private static final String TAG = "arm64bridge";
    private static boolean started;

    private static native void nativeStart();
    private static native void nativeLog(String text);

    @Override
    public boolean onCreate() {
        try {
            System.loadLibrary("arm64bridge");
            nativeStart();
            started = true;
        } catch (Throwable t) {
            Log.e(TAG, "bridge runtime not started early", t);
        }
        new Handler(Looper.getMainLooper()).post(BridgeStarter::loopSurvivingOldApiErrors);
        return true;
    }

    /**
     * Old apps call APIs in ways that newer Android versions refuse with an exception, on the main
     * thread, where it ends the app. Example: Google's first in-app billing library binds to
     * "com.android.vending.billing.MarketBillingService.BIND" with an implicit intent; since
     * Android 5 bindService throws "Service Intent must be explicit" (and that service no longer
     * exists anyway). On the old Android the call just failed. This runs the main loop again after
     * such an exception, so the failing call is skipped and the app goes on (its billing then
     * reports that it is not supported). Any other exception still ends the app as before.
     */
    private static void loopSurvivingOldApiErrors() {
        while (true) {
            try {
                Looper.loop();
                return;  // the loop ended normally (the app is quitting)
            } catch (Throwable t) {
                if (!isOldApiError(t)) throw t;
                String text = "ignored an error of an old API call on the main thread (as on old Android, the call fails): " + t;
                Log.w(TAG, text, t);
                if (started) {
                    try {
                        nativeLog(text);
                    } catch (Throwable ignored) {
                        // logging is best effort
                    }
                }
            }
        }
    }

    private static boolean isOldApiError(Throwable t) {
        if (!(t instanceof IllegalArgumentException)) return false;
        String message = t.getMessage();
        return message != null && message.startsWith("Service Intent must be explicit");
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
