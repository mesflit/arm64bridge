// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

/** Edits of AndroidManifest.xml used by the patcher (see [Axml] for the document model). */
object AxmlPatcher {
    private const val ATTR_LABEL = 0x01010001
    private const val ATTR_NAME = 0x01010003
    private const val ATTR_PERMISSION = 0x01010006
    private const val ATTR_DEBUGGABLE = 0x0101000f
    private const val ATTR_AUTHORITIES = 0x01010018
    private const val ATTR_SCREEN_ORIENTATION = 0x0101001e
    private const val ATTR_MIN_SDK = 0x0101020c
    private const val ATTR_TARGET_ACTIVITY = 0x01010202
    private const val ATTR_ANY_DENSITY = 0x0101026c
    private const val ATTR_TARGET_SDK = 0x01010270
    private const val ATTR_BACKUP_AGENT = 0x0101027f
    private const val ATTR_EXTRACT_NATIVE_LIBS = 0x010104ea
    private const val ATTR_EXPORTED = 0x01010010
    private const val ATTR_INIT_ORDER = 0x0101001a

    /** Provider class (added to the patched APK) that copies embedded OBB/data files on first start. */
    const val DATA_INSTALLER = "com.mesflit.arm64bridge.overlay.DataInstaller"

    /** Provider class that starts the in-game menu in apps that run without the bridge. */
    const val OVERLAY_INSTALLER = "com.mesflit.arm64bridge.overlay.OverlayInstaller"

    private val COMPONENTS = setOf("activity", "activity-alias", "service", "receiver", "provider")
    private val PERMISSION_ELEMENTS = setOf("uses-permission", "uses-permission-sdk-23", "uses-permission-sdk-m")

    class SdkInfo(val minSdk: Int?, val targetSdk: Int?)

    class Info(
        val packageName: String,
        val label: String?,          // only when the manifest holds the label as text
        val permissions: List<String>,
        val minSdk: Int?,
        val targetSdk: Int?,
    )

    /** What to change. null / empty means "leave as it is". */
    data class Edits(
        val raiseTargetSdkTo: Int? = null,
        val disableAnyDensity: Boolean = false,
        val label: String? = null,
        val packageName: String? = null,
        val removePermissions: Set<String> = emptySet(),
        val addPermissions: List<String> = emptyList(),
        val screenOrientation: Int? = null,
        val debuggable: Boolean = false,
        val extractNativeLibs: Boolean = false,
        /** Declare [DATA_INSTALLER]; the patcher sets this when OBB/data files are embedded. */
        val dataInstaller: Boolean = false,
        /** Declare [OVERLAY_INSTALLER]; the patcher sets this for the value editor in apps without the bridge. */
        val overlayInstaller: Boolean = false,
    ) {
        val isEmpty get() = raiseTargetSdkTo == null && !disableAnyDensity && label == null && packageName == null &&
            removePermissions.isEmpty() && addPermissions.isEmpty() && screenOrientation == null && !debuggable &&
            !extractNativeLibs && !dataInstaller && !overlayInstaller
    }

    private fun intValue(a: Axml.Attr?): Int? =
        a?.takeIf { it.type == Axml.TYPE_INT_DEC || it.type == Axml.TYPE_INT_HEX }?.data

    fun info(manifest: ByteArray): Info {
        val doc = Axml.parse(manifest)
        val root = doc.root()
        val app = doc.children(root, "application").firstOrNull()
        val usesSdk = doc.children(root, "uses-sdk").firstOrNull()
        val perms = doc.children(root).filter { it.name in PERMISSION_ELEMENTS }.mapNotNull { it.string(ATTR_NAME) }.distinct()
        return Info(root.attr("package")?.str ?: "", app?.string(ATTR_LABEL), perms,
            intValue(usesSdk?.attr(ATTR_MIN_SDK)), intValue(usesSdk?.attr(ATTR_TARGET_SDK)))
    }

    fun readSdk(manifest: ByteArray): SdkInfo = info(manifest).let { SdkInfo(it.minSdk, it.targetSdk) }

    fun isAnyDensityDisabled(manifest: ByteArray): Boolean {
        val doc = Axml.parse(manifest)
        return doc.children(doc.root(), "supports-screens").any { s ->
            s.attr(ATTR_ANY_DENSITY)?.let { it.type == Axml.TYPE_BOOLEAN && it.data == 0 } == true
        }
    }

    fun apply(manifest: ByteArray, edits: Edits): ByteArray {
        if (edits.isEmpty) return manifest
        val doc = Axml.parse(manifest)
        val root = doc.root()
        require(root.name == "manifest") { "not an Android manifest" }
        val app = doc.children(root, "application").firstOrNull()

        edits.raiseTargetSdkTo?.let { floor ->
            val usesSdk = doc.children(root, "uses-sdk").firstOrNull() ?: doc.insertChild(root, "uses-sdk")
            val target = intValue(usesSdk.attr(ATTR_TARGET_SDK)) ?: intValue(usesSdk.attr(ATTR_MIN_SDK)) ?: 1
            if (target < floor) usesSdk.setInt(ATTR_TARGET_SDK, "targetSdkVersion", floor)
        }

        if (edits.disableAnyDensity) {
            val ss = doc.children(root, "supports-screens").firstOrNull() ?: doc.insertChild(root, "supports-screens")
            ss.setBoolean(ATTR_ANY_DENSITY, "anyDensity", false)
        }

        // Permissions.
        if (edits.removePermissions.isNotEmpty()) {
            for (e in doc.children(root).filter { it.name in PERMISSION_ELEMENTS && it.string(ATTR_NAME) in edits.removePermissions }) {
                doc.remove(e)
            }
        }
        if (edits.addPermissions.isNotEmpty()) {
            val existing = doc.children(root).filter { it.name in PERMISSION_ELEMENTS }.mapNotNull { it.string(ATTR_NAME) }.toSet()
            var after = doc.children(root).lastOrNull { it.name in PERMISSION_ELEMENTS }
            for (p in edits.addPermissions.distinct().filter { it !in existing }) {
                val e = doc.insertChild(root, "uses-permission", after)
                e.setString(ATTR_NAME, "name", p)
                after = e
            }
        }

        // Package name: component class names that are relative to the old package are made absolute,
        // and names that belong to the app itself (authorities, its own permissions) are moved along.
        edits.packageName?.let { newPkg ->
            val pkgAttr = root.attr("package") ?: error("the manifest has no package name")
            val oldPkg = pkgAttr.str ?: error("the manifest has no package name")
            fun absolute(name: String) = when {
                name.startsWith(".") -> oldPkg + name
                !name.contains('.') -> "$oldPkg.$name"
                else -> name
            }
            fun moved(name: String) = if (name == oldPkg || name.startsWith("$oldPkg.")) newPkg + name.substring(oldPkg.length) else name
            for ((e, _) in doc.elements()) {
                when {
                    e.name == "application" -> {
                        e.string(ATTR_NAME)?.let { e.setString(ATTR_NAME, "name", absolute(it)) }
                        e.string(ATTR_BACKUP_AGENT)?.let { e.setString(ATTR_BACKUP_AGENT, "backupAgent", absolute(it)) }
                    }
                    e.name in COMPONENTS -> {
                        e.string(ATTR_NAME)?.let { e.setString(ATTR_NAME, "name", absolute(it)) }
                        e.string(ATTR_TARGET_ACTIVITY)?.let { e.setString(ATTR_TARGET_ACTIVITY, "targetActivity", absolute(it)) }
                        e.string(ATTR_PERMISSION)?.let { e.setString(ATTR_PERMISSION, "permission", moved(it)) }
                        e.string(ATTR_AUTHORITIES)?.let { auth ->
                            e.setString(ATTR_AUTHORITIES, "authorities", auth.split(';').joinToString(";") { moved(it) })
                        }
                    }
                    e.name == "permission" || e.name == "permission-group" || e.name in PERMISSION_ELEMENTS ->
                        e.string(ATTR_NAME)?.let { e.setString(ATTR_NAME, "name", moved(it)) }
                }
            }
            pkgAttr.str = newPkg
            pkgAttr.raw = newPkg
            pkgAttr.type = Axml.TYPE_STRING
        }

        // App name: the application's label and the label of activities that show the same name.
        edits.label?.let { label ->
            requireNotNull(app) { "the manifest has no <application>" }
            val old = app.attr(ATTR_LABEL)?.let { it.type to (it.str ?: it.data.toString()) }
            for ((e, _) in doc.elements()) {
                if (e.name != "activity" && e.name != "activity-alias") continue
                val a = e.attr(ATTR_LABEL) ?: continue
                if (old != null && a.type to (a.str ?: a.data.toString()) == old) e.setString(ATTR_LABEL, "label", label)
            }
            app.setString(ATTR_LABEL, "label", label)
        }

        edits.screenOrientation?.let { o ->
            for ((e, _) in doc.elements()) if (e.name == "activity") e.setInt(ATTR_SCREEN_ORIENTATION, "screenOrientation", o)
        }
        if (edits.debuggable) app?.setBoolean(ATTR_DEBUGGABLE, "debuggable", true)
        if (edits.extractNativeLibs) app?.setBoolean(ATTR_EXTRACT_NATIVE_LIBS, "extractNativeLibs", true)
        val helpers = listOfNotNull(
            if (edits.dataInstaller) DATA_INSTALLER to "data" else null,
            if (edits.overlayInstaller) OVERLAY_INSTALLER to "menu" else null,
        )
        for ((className, suffix) in helpers) {
            requireNotNull(app) { "the manifest has no <application>" }
            val pkg = root.attr("package")?.str ?: error("the manifest has no package name")
            val provider = doc.children(app, "provider").firstOrNull { it.string(ATTR_NAME) == className }
                ?: doc.insertChild(app, "provider")
            provider.setString(ATTR_NAME, "name", className)
            provider.setString(ATTR_AUTHORITIES, "authorities", "$pkg.arm64bridge.$suffix")
            provider.setBoolean(ATTR_EXPORTED, "exported", false)
            // Highest init order: created before the app's other providers (and before the Application).
            provider.setInt(ATTR_INIT_ORDER, "initOrder", Int.MAX_VALUE)
        }

        return doc.toBytes()
    }

    // Kept for existing callers.
    fun raiseTargetSdk(manifest: ByteArray, floor: Int): ByteArray? =
        runCatching { apply(manifest, Edits(raiseTargetSdkTo = floor)) }.getOrNull()

    fun disableAnyDensity(manifest: ByteArray): ByteArray = apply(manifest, Edits(disableAnyDensity = true))

    /** Screen orientation choices: label to android:screenOrientation value. */
    val ORIENTATIONS = listOf(
        "Keep the app's orientation" to null,
        "Landscape (rotates with the device)" to 6,
        "Portrait (rotates with the device)" to 7,
        "Any orientation" to 10,
        "Landscape (fixed)" to 0,
        "Portrait (fixed)" to 1,
    )
}
