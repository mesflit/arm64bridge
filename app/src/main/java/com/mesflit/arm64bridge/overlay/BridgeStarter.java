// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
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
    private static native void nativeStart();

    @Override
    public boolean onCreate() {
        try {
            System.loadLibrary("arm64bridge");
            nativeStart();
        } catch (Throwable t) {
            Log.e("arm64bridge", "bridge runtime not started early", t);
        }
        return true;
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
