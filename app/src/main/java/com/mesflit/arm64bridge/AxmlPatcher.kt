// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * In-place edits of a compiled (binary) AndroidManifest.xml.
 *
 * Recent Android versions refuse to install apps whose targetSdkVersion is very
 * old. The value is stored as a typed integer, so it can be raised without
 * rebuilding the document.
 */
object AxmlPatcher {
    private const val CHUNK_RESOURCE_MAP = 0x0180
    private const val CHUNK_START_ELEMENT = 0x0102
    private const val ATTR_MIN_SDK = 0x0101020c
    private const val ATTR_TARGET_SDK = 0x01010270
    private const val TYPE_INT_DEC = 0x10
    private const val TYPE_INT_HEX = 0x11

    class SdkInfo(val minSdk: Int?, val targetSdk: Int?)

    private class AttrRef(val resId: Int, val typeOffset: Int, val dataOffset: Int)

    private fun attributes(buf: ByteBuffer): List<AttrRef> {
        val out = ArrayList<AttrRef>()
        if (buf.capacity() < 8 || (buf.getShort(0).toInt() and 0xffff) != 0x0003) return out
        var resMap = IntArray(0)
        var pos = buf.getShort(2).toInt() and 0xffff
        while (pos + 8 <= buf.capacity()) {
            val type = buf.getShort(pos).toInt() and 0xffff
            val headerSize = buf.getShort(pos + 2).toInt() and 0xffff
            val size = buf.getInt(pos + 4)
            if (size < 8 || pos + size > buf.capacity()) break
            if (type == CHUNK_RESOURCE_MAP) {
                resMap = IntArray((size - headerSize) / 4) { buf.getInt(pos + headerSize + it * 4) }
            } else if (type == CHUNK_START_ELEMENT) {
                val ext = pos + headerSize
                val attrStart = buf.getShort(ext + 8).toInt() and 0xffff
                val attrSize = buf.getShort(ext + 10).toInt() and 0xffff
                val attrCount = buf.getShort(ext + 12).toInt() and 0xffff
                for (i in 0 until attrCount) {
                    val a = ext + attrStart + i * attrSize
                    if (a + 20 > pos + size) break
                    val nameIdx = buf.getInt(a + 4)
                    if (nameIdx in resMap.indices) out.add(AttrRef(resMap[nameIdx], a + 15, a + 16))
                }
            }
            pos += size
        }
        return out
    }

    private fun intValue(buf: ByteBuffer, ref: AttrRef): Int? {
        val type = buf.get(ref.typeOffset).toInt() and 0xff
        return if (type == TYPE_INT_DEC || type == TYPE_INT_HEX) buf.getInt(ref.dataOffset) else null
    }

    fun readSdk(manifest: ByteArray): SdkInfo {
        val buf = ByteBuffer.wrap(manifest).order(ByteOrder.LITTLE_ENDIAN)
        var min: Int? = null
        var target: Int? = null
        for (ref in attributes(buf)) {
            if (ref.resId == ATTR_MIN_SDK) min = intValue(buf, ref)
            if (ref.resId == ATTR_TARGET_SDK) target = intValue(buf, ref)
        }
        return SdkInfo(min, target)
    }

    /**
     * Returns a copy of the manifest whose effective targetSdkVersion is at least [floor],
     * or null if that is not possible without rewriting the document.
     */
    fun raiseTargetSdk(manifest: ByteArray, floor: Int): ByteArray? {
        val copy = manifest.copyOf()
        val buf = ByteBuffer.wrap(copy).order(ByteOrder.LITTLE_ENDIAN)
        val refs = attributes(buf)
        val target = refs.firstOrNull { it.resId == ATTR_TARGET_SDK && intValue(buf, it) != null }
        if (target != null) {
            if (buf.getInt(target.dataOffset) < floor) buf.putInt(target.dataOffset, floor)
            return copy
        }
        // Without an explicit target the platform uses minSdkVersion.
        val min = refs.firstOrNull { it.resId == ATTR_MIN_SDK && intValue(buf, it) != null } ?: return null
        if (buf.getInt(min.dataOffset) < floor) buf.putInt(min.dataOffset, floor)
        return copy
    }
}
