// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView

/**
 * The permission list of the customization form: one checkbox per permission (unticked = remove) and a
 * field to add more, with suggestions.
 */
class PermissionEditor(
    private val activity: Activity,
    private val list: LinearLayout,
    private val field: AutoCompleteTextView,
) {
    /** Permission name to its "keep" checkbox, in display order. */
    private val boxes = LinkedHashMap<String, CheckBox>()
    private var original: Set<String> = emptySet()

    init {
        field.setAdapter(ArrayAdapter(activity, android.R.layout.simple_dropdown_item_1line, COMMON))
    }

    /** Shows the permissions the app requests now. */
    fun reset(permissions: List<String>) {
        original = permissions.toSet()
        boxes.clear()
        list.removeAllViews()
        permissions.forEach { addRow(it) }
        if (permissions.isEmpty()) list.addView(TextView(activity).apply { setText(R.string.custom_no_permissions) })
    }

    /** Adds the permission typed in the field; "INTERNET" means android.permission.INTERNET. */
    fun addFromField() {
        var name = field.text.toString().trim()
        if (name.isEmpty()) return
        if (!name.contains('.')) name = "android.permission.$name"
        if (!name.matches(PERMISSION_NAME)) {
            field.error = activity.getString(R.string.bad_permission)
            return
        }
        addRow(name)
        field.setText("")
    }

    /** Permissions to remove from and to add to the manifest. */
    fun changes(): Pair<Set<String>, List<String>> {
        val remove = boxes.filter { !it.value.isChecked && it.key in original }.keys
        val add = boxes.filter { it.value.isChecked && it.key !in original }.keys.toList()
        return remove to add
    }

    private fun addRow(name: String) {
        boxes[name]?.let {
            it.isChecked = true
            return
        }
        val box = CheckBox(activity).apply {
            text = name.removePrefix("android.permission.")
            isChecked = true
        }
        boxes[name] = box
        list.addView(box)
    }

    private companion object {
        val PERMISSION_NAME = Regex("^[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z0-9_]+)+$")
        val COMMON = listOf(
            "INTERNET", "ACCESS_NETWORK_STATE", "ACCESS_WIFI_STATE", "VIBRATE", "WAKE_LOCK",
            "READ_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE", "READ_MEDIA_IMAGES", "READ_MEDIA_AUDIO", "READ_MEDIA_VIDEO",
            "POST_NOTIFICATIONS", "CAMERA", "RECORD_AUDIO", "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION",
            "READ_PHONE_STATE", "GET_ACCOUNTS", "BLUETOOTH", "BLUETOOTH_CONNECT", "FOREGROUND_SERVICE",
            "RECEIVE_BOOT_COMPLETED", "SYSTEM_ALERT_WINDOW", "REQUEST_INSTALL_PACKAGES", "SCHEDULE_EXACT_ALARM",
            "com.android.vending.BILLING", "com.android.vending.CHECK_LICENSE",
        ).map { if (it.contains('.')) it else "android.permission.$it" }
    }
}
