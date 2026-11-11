// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.app.PendingIntent
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.provider.Settings
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.ProgressBar
import android.widget.TextView
import java.io.File
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var apkInfo: TextView
    private lateinit var log: TextView
    private lateinit var progress: ProgressBar
    private lateinit var installButton: Button
    private lateinit var saveButton: Button
    private lateinit var raiseTarget: CheckBox
    private lateinit var legacyScaling: CheckBox

    private val inputApk get() = File(cacheDir, "input.apk")
    private val patchedApk get() = File(cacheDir, "patched.apk")
    private var analysis: ApkPatcher.Analysis? = null
    private var displayName = "app"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        apkInfo = view(R.id.apkInfo)
        log = view(R.id.log)
        progress = view(R.id.progress)
        installButton = view(R.id.installButton)
        saveButton = view(R.id.saveButton)
        raiseTarget = view(R.id.raiseTargetSdk)
        legacyScaling = view(R.id.legacyScaling)

        val abis = Build.SUPPORTED_ABIS.joinToString(", ")
        val native32 = Build.SUPPORTED_32_BIT_ABIS.isNotEmpty()
        view<TextView>(R.id.deviceInfo).text = getString(
            R.string.device_info, abis, getString(if (native32) R.string.yes else R.string.no),
            runCatching { NativeBridge.version() }.getOrElse { it.toString() }
        )

        view<Button>(R.id.pickButton).setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/vnd.android.package-archive", "application/octet-stream"))
            }
            startActivityForResult(intent, REQUEST_PICK)
        }
        installButton.setOnClickListener { patchThen { install() } }
        saveButton.setOnClickListener {
            val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "application/vnd.android.package-archive"
                putExtra(Intent.EXTRA_TITLE, displayName.removeSuffix(".apk") + "-arm64bridge.apk")
            }
            startActivityForResult(intent, REQUEST_SAVE)
        }
        view<Button>(R.id.selfTestButton).setOnClickListener { runSelfTest() }

        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        if (intent == null) return
        if (intent.action == ACTION_INSTALL_STATUS) {
            onInstallStatus(intent)
        } else if (intent.action == Intent.ACTION_VIEW && intent.data != null) {
            loadApk(intent.data!!)
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) return
        when (requestCode) {
            REQUEST_PICK -> loadApk(uri)
            REQUEST_SAVE -> patchThen { save(uri) }
        }
    }

    private fun loadApk(uri: Uri) {
        displayName = queryName(uri) ?: "app.apk"
        apkInfo.setText(R.string.reading_apk)
        setBusy(true)
        worker.execute {
            val result = runCatching {
                contentResolver.openInputStream(uri)!!.use { input -> inputApk.outputStream().use { input.copyTo(it, 1 shl 16) } }
                ApkPatcher(this).analyze(inputApk)
            }
            runOnUiThread {
                setBusy(false)
                result.onSuccess { showAnalysis(it) }.onFailure {
                    analysis = null
                    apkInfo.text = getString(R.string.analysis_failed, it.message ?: it.toString())
                    updateButtons()
                }
            }
        }
    }

    private fun showAnalysis(a: ApkPatcher.Analysis) {
        analysis = a
        val sb = StringBuilder()
        val pkg = packageManager.getPackageArchiveInfo(inputApk.path, 0)
        sb.append(displayName)
        if (pkg != null) sb.append("\n").append(pkg.packageName).append("  ").append(pkg.versionName ?: "")
        sb.append("\nminSdk ").append(a.minSdk ?: "?").append(", targetSdk ").append(a.targetSdk ?: a.minSdk ?: "?")
        sb.append("\n\n")
        when {
            a.has64 -> sb.append(getString(R.string.verdict_already64))
            a.abis.isEmpty() -> sb.append(getString(R.string.verdict_no_native))
            a.guestAbi == null -> sb.append(getString(R.string.verdict_not_arm, a.abis.joinToString(", ")))
            else -> {
                sb.append(getString(R.string.verdict_ok, a.guestAbi, a.guestLibs.size))
                sb.append("\n").append(a.guestLibs.joinToString(", ")).append("\n\n")
                if (a.unresolved.isEmpty()) {
                    sb.append(getString(R.string.unresolved_none, a.importCount))
                } else {
                    val shown = a.unresolved.take(40).joinToString(", ") + if (a.unresolved.size > 40) ", …" else ""
                    sb.append(getString(R.string.unresolved_some, a.unresolved.size, a.importCount, shown))
                }
            }
        }
        apkInfo.text = sb
        val floor = ApkPatcher.minInstallableTargetSdk()
        val effectiveTarget = a.targetSdk ?: a.minSdk ?: 1
        val needsRaise = a.patchable && floor > 0 && effectiveTarget < floor
        raiseTarget.visibility = if (needsRaise) View.VISIBLE else View.GONE
        raiseTarget.isChecked = needsRaise
        raiseTarget.text = getString(R.string.raise_target_sdk, floor)
        legacyScaling.visibility = if (a.patchable) View.VISIBLE else View.GONE
        legacyScaling.isChecked = false
        updateButtons()
    }

    private fun updateButtons() {
        val ok = analysis?.patchable == true
        installButton.isEnabled = ok
        saveButton.isEnabled = ok
    }

    private fun setBusy(busy: Boolean) {
        progress.visibility = if (busy) View.VISIBLE else View.INVISIBLE
        progress.isIndeterminate = busy
        if (busy) {
            installButton.isEnabled = false
            saveButton.isEnabled = false
        } else {
            updateButtons()
        }
    }

    /** Produces the patched APK on the worker thread, then runs [next] on the UI thread. */
    private fun patchThen(next: () -> Unit) {
        if (analysis?.patchable != true) return
        val options = ApkPatcher.Options(
            if (raiseTarget.isChecked && raiseTarget.visibility == View.VISIBLE) ApkPatcher.minInstallableTargetSdk() else null,
            legacyScaling.isChecked && legacyScaling.visibility == View.VISIBLE
        )
        setBusy(true)
        progress.isIndeterminate = false
        log.setText(R.string.patching)
        worker.execute {
            val result = runCatching {
                patchedApk.delete()
                ApkPatcher(this).patch(inputApk, patchedApk, options) { p ->
                    runOnUiThread {
                        progress.progress = p
                        if (p >= 85) log.setText(R.string.signing)
                    }
                }
            }
            runOnUiThread {
                setBusy(false)
                result.onSuccess { next() }.onFailure {
                    log.text = getString(R.string.patch_failed, it.message ?: it.toString())
                }
            }
        }
    }

    private fun save(target: Uri) {
        worker.execute {
            val result = runCatching {
                contentResolver.openOutputStream(target)!!.use { out -> patchedApk.inputStream().use { it.copyTo(out, 1 shl 16) } }
            }
            runOnUiThread {
                log.text = result.fold({ getString(R.string.saved) }, { getString(R.string.patch_failed, it.message ?: it.toString()) })
            }
        }
    }

    private fun install() {
        if (!packageManager.canRequestPackageInstalls()) {
            log.setText(R.string.allow_install)
            startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            return
        }
        log.setText(R.string.installing)
        worker.execute {
            val result = runCatching {
                val installer = packageManager.packageInstaller
                val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
                val sessionId = installer.createSession(params)
                installer.openSession(sessionId).use { session ->
                    session.openWrite("base.apk", 0, patchedApk.length()).use { out ->
                        patchedApk.inputStream().use { it.copyTo(out, 1 shl 16) }
                        session.fsync(out)
                    }
                    val callback = Intent(this, MainActivity::class.java).setAction(ACTION_INSTALL_STATUS)
                    val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                        (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                    session.commit(PendingIntent.getActivity(this, sessionId, callback, flags).intentSender)
                }
            }
            result.onFailure { runOnUiThread { log.text = getString(R.string.install_failed, it.message ?: it.toString()) } }
        }
    }

    private fun onInstallStatus(intent: Intent) {
        when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                @Suppress("DEPRECATION")
                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirm != null) startActivity(confirm)
            }
            PackageInstaller.STATUS_SUCCESS -> log.setText(R.string.install_ok)
            else -> log.text = getString(
                R.string.install_failed,
                (intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "") + " (status $status)"
            )
        }
    }

    private fun runSelfTest() {
        log.setText(R.string.self_test_running)
        worker.execute {
            val report = runCatching {
                val dir = File(filesDir, "guest").apply { mkdirs() }
                assets.open("guest/libguestdemo.so").use { input ->
                    File(dir, "libguestdemo.so").outputStream().use { input.copyTo(it) }
                }
                NativeBridge.addGuestDirectory(dir.path)
                NativeBridge.loadGuestLibrary("libguestdemo.so")?.let { error(it) }
                GuestDemo.runAll()
            }.getOrElse { "FAIL\n$it" }
            runOnUiThread { log.text = report }
        }
    }

    private fun <T : View> view(id: Int): T = findViewById<T>(id)!!

    private fun queryName(uri: Uri): String? = runCatching {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    }.getOrNull()

    companion object {
        private const val REQUEST_PICK = 1
        private const val REQUEST_SAVE = 2
        private const val ACTION_INSTALL_STATUS = "com.mesflit.arm64bridge.INSTALL_STATUS"
    }
}
