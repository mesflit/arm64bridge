// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.ProgressBar
import android.widget.TextView
import java.io.File
import java.util.concurrent.Executors

/**
 * The main screen: choose an APK, see what it needs, set the options, then patch and install or save.
 *
 * The parts of the screen live in their own classes: [CustomizeForm] (manifest options and
 * permissions), [ExtrasForm] (OBB and data files), [ApkInstaller] and [AboutDialog]. The work itself
 * is done by [ApkPatcher] on a worker thread.
 */
class MainActivity : Activity() {
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var apkInfo: TextView
    private lateinit var log: TextView
    private lateinit var progress: ProgressBar
    private lateinit var installButton: Button
    private lateinit var saveButton: Button
    private lateinit var fpsBooster: CheckBox
    private lateinit var valueEditor: CheckBox
    private lateinit var customize: CustomizeForm
    private lateinit var extras: ExtrasForm
    private lateinit var installer: ApkInstaller

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
        fpsBooster = view(R.id.inGameMenu)
        valueEditor = view(R.id.valueEditor)
        customize = CustomizeForm(this)
        extras = ExtrasForm(this)
        installer = ApkInstaller(this) { message: String -> log.text = message }

        view<TextView>(R.id.deviceInfo).text = getString(
            R.string.device_info, Build.SUPPORTED_ABIS.joinToString(", "),
            getString(if (Build.SUPPORTED_32_BIT_ABIS.isNotEmpty()) R.string.yes else R.string.no),
            runCatching { NativeBridge.version() }.getOrElse { it.toString() }
        )
        view<Button>(R.id.pickButton).setOnClickListener {
            pick(REQUEST_PICK, Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/vnd.android.package-archive", "application/octet-stream"))
            })
        }
        installButton.setOnClickListener { patchThen { worker.execute { installer.install(patchedApk) } } }
        saveButton.setOnClickListener {
            pick(REQUEST_SAVE, Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "application/vnd.android.package-archive"
                putExtra(Intent.EXTRA_TITLE, displayName.removeSuffix(".apk") + "-arm64bridge.apk")
            })
        }
        view<Button>(R.id.selfTestButton).setOnClickListener { runSelfTest() }
        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menu.add(Menu.NONE, MENU_ABOUT, Menu.NONE, R.string.about)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId != MENU_ABOUT) return super.onOptionsItemSelected(item)
        AboutDialog.show(this)
        return true
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        if (extras.onResult(requestCode, data)) return
        val uri = data.data ?: return
        when (requestCode) {
            REQUEST_PICK -> loadApk(uri)
            REQUEST_SAVE -> patchThen { save(uri) }
        }
    }

    private fun handleIntent(intent: Intent?) {
        when {
            intent == null -> return
            intent.action == ApkInstaller.ACTION_STATUS -> installer.onStatus(intent)
            intent.action == Intent.ACTION_VIEW && intent.data != null -> loadApk(intent.data!!)
        }
    }

    @Suppress("DEPRECATION")
    private fun pick(requestCode: Int, intent: Intent) = startActivityForResult(intent, requestCode)

    // ------------------------------------------------------------------ 1. choose and analyze

    private fun loadApk(uri: Uri) {
        displayName = contentResolver.displayName(uri) ?: "app.apk"
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
        apkInfo.text = describe(a)
        fpsBooster.visibility = if (a.needsBridge) View.VISIBLE else View.GONE
        fpsBooster.isChecked = false
        valueEditor.visibility = if (a.customizable) View.VISIBLE else View.GONE
        valueEditor.isChecked = false
        installButton.setText(if (a.needsBridge) R.string.patch_install else R.string.apply_install)
        saveButton.setText(if (a.needsBridge) R.string.patch_save else R.string.apply_save)
        customize.reset(a, inputApk)
        extras.reset()
        updateButtons()
    }

    /** What the APK is and what the bridge can do with it. */
    private fun describe(a: ApkPatcher.Analysis): String {
        val sb = StringBuilder(displayName)
        packageManager.getPackageArchiveInfo(inputApk.path, 0)?.let { sb.append("\n").append(it.packageName).append("  ").append(it.versionName ?: "") }
        sb.append("\nminSdk ").append(a.minSdk ?: "?").append(", targetSdk ").append(a.targetSdk ?: a.minSdk ?: "?").append("\n\n")
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
        return sb.toString()
    }

    // ------------------------------------------------------------------ 2. patch, then install or save

    /** Produces the patched APK on the worker thread, then runs [next] on the UI thread. */
    private fun patchThen(next: () -> Unit) {
        val a = analysis ?: return
        if (!a.customizable) return
        val edits = customize.edits(a) ?: return
        val options = ApkPatcher.Options(edits, a.needsBridge && fpsBooster.isChecked, extras.chosen, valueEditor.isChecked)
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
                result.onSuccess { next() }.onFailure { log.text = getString(R.string.patch_failed, it.message ?: it.toString()) }
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

    // ------------------------------------------------------------------ self test and state

    private fun runSelfTest() {
        log.setText(R.string.self_test_running)
        worker.execute {
            val report = runCatching {
                val dir = File(filesDir, "guest").apply { mkdirs() }
                assets.open("guest/libguestdemo.so").use { input -> File(dir, "libguestdemo.so").outputStream().use { input.copyTo(it) } }
                NativeBridge.addGuestDirectory(dir.path)
                NativeBridge.loadGuestLibrary("libguestdemo.so")?.let { error(it) }
                GuestDemo.runAll()
            }.getOrElse { "FAIL\n$it" }
            runOnUiThread { log.text = report }
        }
    }

    private fun updateButtons() {
        val ok = analysis?.customizable == true
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

    private companion object {
        const val REQUEST_PICK = 1
        const val REQUEST_SAVE = 2
        const val MENU_ABOUT = 1
    }
}
