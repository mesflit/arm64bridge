// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.app.Application;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

/**
 * Starts the in-game menu in apps that run without the bridge (native 64-bit apps that were only
 * customized). Not used by the ARM64Bridge app itself: the patcher adds this class to the patched APK
 * and declares it as a content provider, because providers are created when the app starts. Apps that
 * run on the bridge get the menu from the bridge runtime instead.
 */
public final class OverlayInstaller extends ContentProvider {
    @Override
    public boolean onCreate() {
        try {
            Context context = getContext();
            Context app = context == null ? null : context.getApplicationContext();
            if (app instanceof Application) GameOverlay.installStandalone((Application) app);
        } catch (Throwable t) {
            Log.e("arm64bridge", "in-game menu not installed", t);
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
