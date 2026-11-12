// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.app.PendingIntent
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import java.io.File

/**
 * Installs an APK with [PackageInstaller]. The installer reports back by starting [Activity] again with
 * [ACTION_STATUS]; that intent goes to [onStatus]. [report] shows a message to the user.
 */
class ApkInstaller(private val activity: Activity, private val report: (String) -> Unit) {

    /** Runs on a worker thread. Returns false if the user first has to allow installing apps. */
    fun install(apk: File): Boolean {
        val pm = activity.packageManager
        if (!pm.canRequestPackageInstalls()) {
            activity.runOnUiThread {
                report(activity.getString(R.string.allow_install))
                activity.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${activity.packageName}")))
            }
            return false
        }
        activity.runOnUiThread { report(activity.getString(R.string.installing)) }
        runCatching {
            val installer = pm.packageInstaller
            val sessionId = installer.createSession(PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL))
            installer.openSession(sessionId).use { session ->
                session.openWrite("base.apk", 0, apk.length()).use { out ->
                    apk.inputStream().use { it.copyTo(out, 1 shl 16) }
                    session.fsync(out)
                }
                val callback = Intent(activity, activity.javaClass).setAction(ACTION_STATUS)
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                session.commit(PendingIntent.getActivity(activity, sessionId, callback, flags).intentSender)
            }
        }.onFailure { e ->
            activity.runOnUiThread { report(activity.getString(R.string.install_failed, e.message ?: e.toString())) }
        }
        return true
    }

    fun onStatus(intent: Intent) {
        when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                @Suppress("DEPRECATION")
                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirm != null) activity.startActivity(confirm)
            }
            PackageInstaller.STATUS_SUCCESS -> report(activity.getString(R.string.install_ok))
            else -> report(activity.getString(R.string.install_failed,
                (intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "") + " (status $status)"))
        }
    }

    companion object {
        const val ACTION_STATUS = "com.mesflit.arm64bridge.INSTALL_STATUS"
    }
}
