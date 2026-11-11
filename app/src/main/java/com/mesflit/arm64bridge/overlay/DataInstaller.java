// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Copies OBB and data files that the patcher embedded in the APK to the app's own storage
 * (Android/obb/&lt;package&gt; and Android/data/&lt;package&gt;). Not used by the ARM64Bridge app itself:
 * the patcher adds this class to the patched APK and declares it as a content provider, because
 * providers are created before the app's own code runs. An app may always write to its own
 * Android/obb and Android/data folders, so no permission is needed. Only framework classes are used.
 */
public final class DataInstaller extends ContentProvider {
    private static final String TAG = "arm64bridge";
    static final String OBB_PREFIX = "assets/arm64bridge/obb/";
    static final String DATA_PREFIX = "assets/arm64bridge/data/";

    @Override
    public boolean onCreate() {
        Context context = getContext();
        if (context == null) return true;
        try {
            install(context);
        } catch (Throwable t) {
            Log.e(TAG, "copying the embedded OBB/data files failed", t);
        }
        return true;
    }

    private static void install(Context context) throws Exception {
        File obbDir = context.getObbDir();
        File filesDir = context.getExternalFilesDir(null);
        File dataDir = filesDir != null ? filesDir.getParentFile() : null;
        long start = System.currentTimeMillis();
        int copied = 0;
        long bytes = 0;
        try (ZipFile apk = new ZipFile(context.getApplicationInfo().sourceDir)) {
            byte[] buffer = new byte[1 << 20];
            Enumeration<? extends ZipEntry> entries = apk.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                if (entry.isDirectory()) continue;
                File base;
                String relative;
                if (name.startsWith(OBB_PREFIX)) {
                    base = obbDir;
                    relative = name.substring(OBB_PREFIX.length());
                } else if (name.startsWith(DATA_PREFIX)) {
                    base = dataDir;
                    relative = name.substring(DATA_PREFIX.length());
                } else {
                    continue;
                }
                if (base == null || relative.isEmpty() || relative.startsWith("/")
                        || ("/" + relative + "/").contains("/../")) continue;
                File target = new File(base, relative);
                long size = entry.getSize();
                if (target.isFile() && (size < 0 || target.length() == size)) continue;
                File parent = target.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                    Log.w(TAG, "cannot create " + parent);
                    continue;
                }
                File tmp = new File(target.getPath() + ".a64b-tmp");
                try (InputStream in = apk.getInputStream(entry); FileOutputStream out = new FileOutputStream(tmp)) {
                    int n;
                    while ((n = in.read(buffer)) > 0) {
                        out.write(buffer, 0, n);
                        bytes += n;
                    }
                    out.getFD().sync();
                }
                if (!tmp.renameTo(target)) {
                    target.delete();
                    if (!tmp.renameTo(target)) throw new java.io.IOException("cannot write " + target);
                }
                copied++;
            }
        }
        if (copied > 0) {
            Log.i(TAG, "copied " + copied + " embedded file(s), " + (bytes >> 20) + " MiB, in "
                    + (System.currentTimeMillis() - start) + " ms");
        }
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
