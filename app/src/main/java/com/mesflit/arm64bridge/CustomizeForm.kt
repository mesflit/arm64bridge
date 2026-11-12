// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.Spinner
import java.io.File

/**
 * The manifest options of the main screen: target SDK, low-resolution rendering, app name, package
 * name, screen orientation, debuggable, native library extraction and permissions. Turns them into
 * [AxmlPatcher.Edits].
 */
class CustomizeForm(private val activity: Activity) {
    private val raiseTarget: CheckBox = activity.view(R.id.raiseTargetSdk)
    private val legacyScaling: CheckBox = activity.view(R.id.legacyScaling)
    private val section: View = activity.view(R.id.customize)
    private val label: EditText = activity.view(R.id.appLabel)
    private val packageName: EditText = activity.view(R.id.appPackage)
    private val orientation: Spinner = activity.view(R.id.orientation)
    private val debuggable: CheckBox = activity.view(R.id.debuggable)
    private val extractLibs: CheckBox = activity.view(R.id.extractNativeLibs)
    private val permissions = PermissionEditor(activity, activity.view(R.id.permissionList), activity.view(R.id.newPermission))

    init {
        orientation.adapter = ArrayAdapter(activity, android.R.layout.simple_spinner_dropdown_item, AxmlPatcher.ORIENTATIONS.map { it.first })
        activity.view<Button>(R.id.addPermission).setOnClickListener { permissions.addFromField() }
    }

    /** Fills the form for a newly chosen APK. */
    fun reset(a: ApkPatcher.Analysis, apk: File) {
        val floor = ApkPatcher.minInstallableTargetSdk()
        val needsRaise = a.customizable && floor > 0 && (a.targetSdk ?: a.minSdk ?: 1) < floor
        raiseTarget.visibility = if (needsRaise) View.VISIBLE else View.GONE
        raiseTarget.isChecked = needsRaise
        raiseTarget.text = activity.getString(R.string.raise_target_sdk, floor)
        legacyScaling.visibility = if (a.customizable) View.VISIBLE else View.GONE
        legacyScaling.isChecked = false

        val info = a.manifest
        label.setText("")
        label.hint = info?.label ?: activity.getString(R.string.label_is_resource, labelFromResources(apk) ?: "?")
        packageName.setText("")
        packageName.hint = info?.packageName ?: ""
        packageName.error = null
        orientation.setSelection(0)
        debuggable.isChecked = false
        extractLibs.isChecked = false
        permissions.reset(info?.permissions ?: emptyList())
        section.visibility = if (a.customizable) View.VISIBLE else View.GONE
    }

    /** The chosen changes, or null when a field holds an invalid value (the field then shows the error). */
    fun edits(a: ApkPatcher.Analysis): AxmlPatcher.Edits? {
        val pkg = packageName.text.toString().trim().takeIf { it.isNotEmpty() && it != a.manifest?.packageName }
        if (pkg != null && !pkg.matches(PACKAGE_NAME)) {
            packageName.error = activity.getString(R.string.bad_package)
            packageName.requestFocus()
            return null
        }
        val newLabel = label.text.toString().trim().takeIf { it.isNotEmpty() && it != a.manifest?.label }
        val (remove, add) = permissions.changes()
        return AxmlPatcher.Edits(
            raiseTargetSdkTo = if (raiseTarget.isChecked && raiseTarget.visibility == View.VISIBLE) ApkPatcher.minInstallableTargetSdk() else null,
            disableAnyDensity = legacyScaling.isChecked,
            label = newLabel,
            packageName = pkg,
            removePermissions = remove,
            addPermissions = add,
            screenOrientation = AxmlPatcher.ORIENTATIONS.getOrNull(orientation.selectedItemPosition)?.second,
            debuggable = debuggable.isChecked,
            extractNativeLibs = extractLibs.isChecked,
        )
    }

    /** The app name when the manifest takes it from a string resource. */
    private fun labelFromResources(apk: File): String? {
        val pm = activity.packageManager
        val app = pm.getPackageArchiveInfo(apk.path, 0)?.applicationInfo ?: return null
        app.sourceDir = apk.path
        app.publicSourceDir = apk.path
        return runCatching { pm.getApplicationLabel(app).toString() }.getOrNull()
    }

    private companion object {
        val PACKAGE_NAME = Regex("^[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)+$")
    }
}
