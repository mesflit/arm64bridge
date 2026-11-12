// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.content.ContentResolver
import android.net.Uri
import android.provider.OpenableColumns

/** The file name the document provider shows for [uri], or null. */
fun ContentResolver.displayName(uri: Uri): String? = runCatching {
    query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
        if (c.moveToFirst()) c.getString(0) else null
    }
}.getOrNull()
