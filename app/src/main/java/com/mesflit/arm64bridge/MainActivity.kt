// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.provider.Settings
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.TextView
import java.io.File
import java.io.FilterInputStream
import java.util.zip.ZipInputStream
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
    private lateinit var inGameMenu: CheckBox
    private lateinit var valueEditor: CheckBox
    private lateinit var customize: View
    private lateinit var appLabel: EditText
    private lateinit var appPackage: EditText
    private lateinit var orientation: Spinner
    private lateinit var debuggable: CheckBox
    private lateinit var extractLibs: CheckBox
    private lateinit var permissionList: LinearLayout
    private lateinit var newPermission: AutoCompleteTextView
    /** Permission name to "keep" checkbox, in display order. */
    private val permissionBoxes = LinkedHashMap<String, CheckBox>()
    private lateinit var extrasList: TextView
    private lateinit var clearExtras: Button
    /** Chosen OBB/data sources: description shown to the user, and the patcher input. */
    private val extras = ArrayList<Pair<String, ApkPatcher.Extra>>()

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
        inGameMenu = view(R.id.inGameMenu)
        valueEditor = view(R.id.valueEditor)
        customize = view(R.id.customize)
        appLabel = view(R.id.appLabel)
        appPackage = view(R.id.appPackage)
        orientation = view(R.id.orientation)
        debuggable = view(R.id.debuggable)
        extractLibs = view(R.id.extractNativeLibs)
        permissionList = view(R.id.permissionList)
        newPermission = view(R.id.newPermission)
        orientation.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, AxmlPatcher.ORIENTATIONS.map { it.first })
        newPermission.setAdapter(ArrayAdapter(this, android.R.layout.simple_dropdown_item_1line, COMMON_PERMISSIONS))
        view<Button>(R.id.addPermission).setOnClickListener { addPermissionFromField() }
        extrasList = view(R.id.extrasList)
        clearExtras = view(R.id.clearExtras)
        clearExtras.setOnClickListener { extras.clear(); showExtras() }
        view<Button>(R.id.addObb).setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            }, REQUEST_OBB)
        }
        view<Button>(R.id.addDataZip).setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
            }, REQUEST_DATA_ZIP)
        }
        view<Button>(R.id.addDataFolder).setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_DATA_FOLDER)
        }

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
        if (resultCode != RESULT_OK || data == null) return
        if (requestCode == REQUEST_OBB) {
            val uris = data.clipData?.let { c -> (0 until c.itemCount).map { c.getItemAt(it).uri } } ?: listOfNotNull(data.data)
            for (u in uris) addObb(u)
            showExtras()
            return
        }
        val uri = data.data ?: return
        when (requestCode) {
            REQUEST_DATA_ZIP -> { addDataZip(uri); showExtras() }
            REQUEST_DATA_FOLDER -> { addDataFolder(uri); showExtras() }
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

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menu.add(Menu.NONE, MENU_ABOUT, Menu.NONE, R.string.about)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId != MENU_ABOUT) return super.onOptionsItemSelected(item)
        val version = runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull() ?: "?"
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.app_name))
            .setIcon(R.drawable.ic_launcher)
            .setMessage(getString(R.string.about_text, version) + "\n\n" + getString(R.string.project_url))
            .setPositiveButton(R.string.about_close, null)
            .setNeutralButton(R.string.about_source) { _, _ ->
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(getString(R.string.project_url)))) }
            }
            .show()
        return true
    }

    private fun addPermissionRow(name: String, keep: Boolean) {
        if (permissionBoxes.containsKey(name)) {
            permissionBoxes[name]!!.isChecked = true
            return
        }
        val box = CheckBox(this).apply {
            text = name.removePrefix("android.permission.")
            isChecked = keep
        }
        permissionBoxes[name] = box
        permissionList.addView(box)
    }

    private fun addPermissionFromField() {
        var name = newPermission.text.toString().trim()
        if (name.isEmpty()) return
        if (!name.contains('.')) name = "android.permission.$name"
        if (!name.matches(PERMISSION_REGEX)) {
            newPermission.error = getString(R.string.bad_permission)
            return
        }
        addPermissionRow(name, true)
        newPermission.setText("")
    }

    private fun addObb(uri: Uri) {
        val name = queryName(uri) ?: return
        extras += getString(R.string.extras_obb, name) to ApkPatcher.Extra(ApkPatcher.Extra.Kind.OBB) { emit ->
            contentResolver.openInputStream(uri)!!.use { emit(name, it) }
        }
    }

    private fun addDataZip(uri: Uri) {
        val name = queryName(uri) ?: "data.zip"
        extras += getString(R.string.extras_data_zip, name) to ApkPatcher.Extra(ApkPatcher.Extra.Kind.DATA) { emit ->
            ZipInputStream(contentResolver.openInputStream(uri)!!.buffered(1 shl 16)).use { zip ->
                val entryStream = object : FilterInputStream(zip) { override fun close() {} }
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (!entry.isDirectory) emit(entry.name, entryStream)
                }
            }
        }
    }

    private fun addDataFolder(tree: Uri) {
        val root = DocumentsContract.getTreeDocumentId(tree)
        val name = root.substringAfterLast('/').substringAfterLast(':')
        extras += getString(R.string.extras_data_folder, name) to ApkPatcher.Extra(ApkPatcher.Extra.Kind.DATA) { emit ->
            fun walk(docId: String, prefix: String) {
                val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
                val cols = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE)
                val items = ArrayList<Triple<String, String, Boolean>>()
                contentResolver.query(children, cols, null, null, null)?.use { c ->
                    while (c.moveToNext()) items += Triple(c.getString(0), c.getString(1), c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR)
                }
                for ((id, child, isDir) in items) {
                    if (isDir) {
                        walk(id, "$prefix$child/")
                    } else {
                        val doc = DocumentsContract.buildDocumentUriUsingTree(tree, id)
                        contentResolver.openInputStream(doc)!!.use { emit(prefix + child, it) }
                    }
                }
            }
            // The chosen folder is the package folder (or a folder inside it, such as "files").
            walk(root, if (name == "files" || name == "cache") "$name/" else "")
        }
    }

    private fun showExtras() {
        extrasList.text = if (extras.isEmpty()) getString(R.string.extras_none) else extras.joinToString("\n") { it.first }
        clearExtras.visibility = if (extras.isEmpty()) View.GONE else View.VISIBLE
    }

    private fun resetCustomization(a: ApkPatcher.Analysis) {
        val info = a.manifest
        appLabel.setText("")
        appLabel.hint = info?.label ?: getString(R.string.label_is_resource, packageManager
            .getPackageArchiveInfo(inputApk.path, 0)?.applicationInfo
            ?.also { it.sourceDir = inputApk.path; it.publicSourceDir = inputApk.path }
            ?.let { runCatching { packageManager.getApplicationLabel(it).toString() }.getOrNull() } ?: "?")
        appPackage.setText("")
        appPackage.hint = info?.packageName ?: ""
        appPackage.error = null
        orientation.setSelection(0)
        debuggable.isChecked = false
        extractLibs.isChecked = false
        permissionBoxes.clear()
        permissionList.removeAllViews()
        info?.permissions?.forEach { addPermissionRow(it, true) }
        extras.clear()
        showExtras()
        if (info?.permissions.isNullOrEmpty()) {
            permissionList.addView(TextView(this).apply { setText(R.string.custom_no_permissions) })
        }
        customize.visibility = if (a.customizable) View.VISIBLE else View.GONE
    }

    /** null when the form holds an invalid value (the field shows the error). */
    private fun collectEdits(a: ApkPatcher.Analysis): AxmlPatcher.Edits? {
        val pkg = appPackage.text.toString().trim().takeIf { it.isNotEmpty() && it != a.manifest?.packageName }
        if (pkg != null && !pkg.matches(PACKAGE_REGEX)) {
            appPackage.error = getString(R.string.bad_package)
            appPackage.requestFocus()
            return null
        }
        val label = appLabel.text.toString().trim().takeIf { it.isNotEmpty() && it != a.manifest?.label }
        val original = a.manifest?.permissions?.toSet() ?: emptySet()
        val remove = permissionBoxes.filter { !it.value.isChecked && it.key in original }.keys
        val add = permissionBoxes.filter { it.value.isChecked && it.key !in original }.keys.toList()
        return AxmlPatcher.Edits(
            raiseTargetSdkTo = if (raiseTarget.isChecked && raiseTarget.visibility == View.VISIBLE) ApkPatcher.minInstallableTargetSdk() else null,
            disableAnyDensity = legacyScaling.isChecked,
            label = label,
            packageName = pkg,
            removePermissions = remove,
            addPermissions = add,
            screenOrientation = AxmlPatcher.ORIENTATIONS.getOrNull(orientation.selectedItemPosition)?.second,
            debuggable = debuggable.isChecked,
            extractNativeLibs = extractLibs.isChecked,
        )
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
        val needsRaise = a.customizable && floor > 0 && effectiveTarget < floor
        raiseTarget.visibility = if (needsRaise) View.VISIBLE else View.GONE
        raiseTarget.isChecked = needsRaise
        raiseTarget.text = getString(R.string.raise_target_sdk, floor)
        legacyScaling.visibility = if (a.customizable) View.VISIBLE else View.GONE
        legacyScaling.isChecked = false
        inGameMenu.visibility = if (a.needsBridge) View.VISIBLE else View.GONE
        inGameMenu.isChecked = false
        valueEditor.visibility = if (a.customizable) View.VISIBLE else View.GONE
        valueEditor.isChecked = false
        installButton.setText(if (a.needsBridge) R.string.patch_install else R.string.apply_install)
        saveButton.setText(if (a.needsBridge) R.string.patch_save else R.string.apply_save)
        resetCustomization(a)
        updateButtons()
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

    /** Produces the patched APK on the worker thread, then runs [next] on the UI thread. */
    private fun patchThen(next: () -> Unit) {
        val a = analysis ?: return
        if (!a.customizable) return
        val edits = collectEdits(a) ?: return
        val options = ApkPatcher.Options(edits, a.needsBridge && inGameMenu.isChecked, extras.map { it.second }, valueEditor.isChecked)
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
        private const val REQUEST_OBB = 3
        private const val REQUEST_DATA_ZIP = 4
        private const val REQUEST_DATA_FOLDER = 5
        private const val ACTION_INSTALL_STATUS = "com.mesflit.arm64bridge.INSTALL_STATUS"
        private const val MENU_ABOUT = 1
        private val PACKAGE_REGEX = Regex("^[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)+$")
        private val PERMISSION_REGEX = Regex("^[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z0-9_]+)+$")
        private val COMMON_PERMISSIONS = listOf(
            "INTERNET", "ACCESS_NETWORK_STATE", "ACCESS_WIFI_STATE", "VIBRATE", "WAKE_LOCK",
            "READ_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE", "READ_MEDIA_IMAGES", "READ_MEDIA_AUDIO", "READ_MEDIA_VIDEO",
            "POST_NOTIFICATIONS", "CAMERA", "RECORD_AUDIO", "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION",
            "READ_PHONE_STATE", "GET_ACCOUNTS", "BLUETOOTH", "BLUETOOTH_CONNECT", "FOREGROUND_SERVICE",
            "RECEIVE_BOOT_COMPLETED", "SYSTEM_ALERT_WINDOW", "REQUEST_INSTALL_PACKAGES", "SCHEDULE_EXACT_ALARM",
            "com.android.vending.BILLING", "com.android.vending.CHECK_LICENSE",
        ).map { if (it.contains('.')) it else "android.permission.$it" }
    }
}
