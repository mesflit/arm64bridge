// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.net.Uri

/** The "About" dialog: version, what the app does, licence and a link to the project page. */
object AboutDialog {
    fun show(activity: Activity) {
        val version = runCatching { activity.packageManager.getPackageInfo(activity.packageName, 0).versionName }.getOrNull() ?: "?"
        val url = activity.getString(R.string.project_url)
        AlertDialog.Builder(activity)
            .setTitle(activity.getString(R.string.app_name))
            .setIcon(R.drawable.ic_launcher)
            .setMessage(activity.getString(R.string.about_text, version) + "\n\n" + url)
            .setPositiveButton(R.string.about_close, null)
            .setNeutralButton(R.string.about_source) { _, _ ->
                runCatching { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
            }
            .show()
    }
}
