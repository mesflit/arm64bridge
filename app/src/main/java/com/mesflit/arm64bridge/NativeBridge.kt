// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

/** JNI entry points of libarm64bridge.so (see core/src/api.cpp). */
object NativeBridge {
    init {
        System.loadLibrary("arm64bridge")
    }

    /** Describes a native library: "key=value" lines (arm32, machine, needed, imports, unresolved, export). */
    @JvmStatic external fun inspectElf(data: ByteArray): String

    /** Adds a directory that contains 32-bit ARM libraries. */
    @JvmStatic external fun addGuestDirectory(dir: String)

    /** Loads a 32-bit library through the bridge and binds its native methods. Returns null or an error. */
    @JvmStatic external fun loadGuestLibrary(name: String): String?

    @JvmStatic external fun version(): String
}
