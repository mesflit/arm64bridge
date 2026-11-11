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

    // ------------------------------------------------------------------------------------------
    // Legacy screen scaling: <supports-screens android:anyDensity="false"/> makes Android run the
    // app in screen compatibility mode. The app then sees a medium-density screen (fewer pixels),
    // and the system scales its output and touch input to the real display.
    // ------------------------------------------------------------------------------------------
    private const val ATTR_ANY_DENSITY = 0x0101026c
    private const val CHUNK_STRING_POOL = 0x0001
    private const val CHUNK_START_NS = 0x0100
    private const val CHUNK_END_NS = 0x0101
    private const val CHUNK_END_ELEMENT = 0x0103
    private const val CHUNK_CDATA = 0x0104
    private const val TYPE_STRING = 0x03
    private const val TYPE_BOOLEAN = 0x12
    private const val ANDROID_NS = "http://schemas.android.com/apk/res/android"

    fun isAnyDensityDisabled(manifest: ByteArray): Boolean {
        val buf = ByteBuffer.wrap(manifest).order(ByteOrder.LITTLE_ENDIAN)
        return attributes(buf).any { it.resId == ATTR_ANY_DENSITY && buf.getInt(it.dataOffset) == 0 }
    }

    /** Returns a manifest that declares android:anyDensity="false". Throws if the document cannot be edited. */
    fun disableAnyDensity(manifest: ByteArray): ByteArray {
        val src = ByteBuffer.wrap(manifest).order(ByteOrder.LITTLE_ENDIAN)
        require(manifest.size >= 16 && (src.getShort(0).toInt() and 0xffff) == 0x0003) { "not a binary XML document" }

        // ---- string pool ----
        val poolPos = src.getShort(2).toInt() and 0xffff
        require((src.getShort(poolPos).toInt() and 0xffff) == CHUNK_STRING_POOL) { "string pool not found" }
        val poolHeader = src.getShort(poolPos + 2).toInt() and 0xffff
        val poolSize = src.getInt(poolPos + 4)
        val stringCount = src.getInt(poolPos + 8)
        require(src.getInt(poolPos + 12) == 0) { "styled strings are not supported" }
        val flags = src.getInt(poolPos + 16)
        val utf8 = flags and 0x100 != 0
        val stringsStart = poolPos + src.getInt(poolPos + 20)
        val blobs = ArrayList<ByteArray>(stringCount + 2)
        val texts = ArrayList<String>(stringCount + 2)
        for (i in 0 until stringCount) {
            val start = stringsStart + src.getInt(poolPos + poolHeader + i * 4)
            var p = start
            val text: String
            if (utf8) {
                p += if (src.get(p).toInt() and 0x80 != 0) 2 else 1 // length in characters
                var bytes = src.get(p).toInt() and 0xff
                if (bytes and 0x80 != 0) { bytes = ((bytes and 0x7f) shl 8) or (src.get(p + 1).toInt() and 0xff); p += 2 } else p += 1
                text = String(manifest, p, bytes, Charsets.UTF_8)
                p += bytes + 1
            } else {
                var units = src.getShort(p).toInt() and 0xffff
                p += 2
                if (units and 0x8000 != 0) { units = ((units and 0x7fff) shl 16) or (src.getShort(p).toInt() and 0xffff); p += 2 }
                text = String(manifest, p, units * 2, Charsets.UTF_16LE)
                p += units * 2 + 2
            }
            blobs.add(manifest.copyOfRange(start, p))
            texts.add(text)
        }
        fun encode(text: String): ByteArray = if (utf8) {
            val b = text.toByteArray(Charsets.UTF_8)
            byteArrayOf(text.length.toByte(), b.size.toByte()) + b + byteArrayOf(0)
        } else {
            val b = text.toByteArray(Charsets.UTF_16LE)
            byteArrayOf(text.length.toByte(), 0) + b + byteArrayOf(0, 0)
        }

        // ---- resource map ----
        val mapPos = poolPos + poolSize
        require((src.getShort(mapPos).toInt() and 0xffff) == CHUNK_RESOURCE_MAP) { "resource map not found" }
        val mapHeader = src.getShort(mapPos + 2).toInt() and 0xffff
        val mapSize = src.getInt(mapPos + 4)
        val ids = ArrayList<Int>()
        for (i in 0 until (mapSize - mapHeader) / 4) ids.add(src.getInt(mapPos + mapHeader + i * 4))

        val hasElement = manifestHasElement(src, mapPos + mapSize, manifest.size, texts)

        // The attribute name must be one of the strings covered by the resource map. If it is new,
        // it is inserted right after them, which moves every later string up by one.
        var attrName = ids.indexOf(ATTR_ANY_DENSITY)
        var insertedAt = -1
        if (attrName < 0) {
            require(ids.size <= texts.size) { "inconsistent resource map" }
            insertedAt = ids.size
            attrName = insertedAt
            ids.add(ATTR_ANY_DENSITY)
            texts.add(insertedAt, "anyDensity")
            blobs.add(insertedAt, encode("anyDensity"))
        }
        fun shift(index: Int) = if (insertedAt >= 0 && index != -1 && index >= insertedAt) index + 1 else index
        var elementName = texts.indexOf("supports-screens")
        if (elementName < 0) {
            elementName = texts.size
            texts.add("supports-screens")
            blobs.add(encode("supports-screens"))
        }
        val nsIndex = texts.indexOf(ANDROID_NS)
        require(nsIndex >= 0) { "android namespace not found" }

        // ---- XML nodes ----
        val out = java.io.ByteArrayOutputStream(manifest.size + 256)
        fun le(vararg values: Int): ByteArray {
            val b = ByteBuffer.allocate(values.size * 4).order(ByteOrder.LITTLE_ENDIAN)
            values.forEach { b.putInt(it) }
            return b.array()
        }
        fun attribute(): ByteArray = le(nsIndex, attrName, -1, (TYPE_BOOLEAN shl 24) or 8, 0)

        var pos = mapPos + mapSize
        var done = false
        var firstElementSeen = false
        while (pos + 8 <= manifest.size) {
            val type = src.getShort(pos).toInt() and 0xffff
            val size = src.getInt(pos + 4)
            require(size >= 8 && pos + size <= manifest.size) { "corrupt chunk" }
            val chunk = manifest.copyOfRange(pos, pos + size)
            val c = ByteBuffer.wrap(chunk).order(ByteOrder.LITTLE_ENDIAN)
            fun fix(offset: Int) = c.putInt(offset, shift(c.getInt(offset)))
            when (type) {
                CHUNK_START_NS, CHUNK_END_NS, CHUNK_END_ELEMENT -> { fix(12); fix(16); fix(20) }
                CHUNK_CDATA -> { fix(12); fix(16); if ((c.get(23).toInt() and 0xff) == TYPE_STRING) fix(24) }
                CHUNK_START_ELEMENT -> {
                    fix(12); fix(16); fix(20)
                    val attrStart = 16 + (c.getShort(24).toInt() and 0xffff)
                    val attrSize = c.getShort(26).toInt() and 0xffff
                    val count = c.getShort(28).toInt() and 0xffff
                    for (i in 0 until count) {
                        val a = attrStart + i * attrSize
                        fix(a); fix(a + 4); fix(a + 8)
                        if ((c.get(a + 15).toInt() and 0xff) == TYPE_STRING) fix(a + 16)
                    }
                }
            }
            if (type == CHUNK_START_ELEMENT && c.getInt(20) == elementName) {
                // An existing <supports-screens>: set the attribute or add it (attributes stay sorted by resource ID).
                val attrStart = 16 + (c.getShort(24).toInt() and 0xffff)
                val attrSize = c.getShort(26).toInt() and 0xffff
                val count = c.getShort(28).toInt() and 0xffff
                require(attrSize == 20) { "unexpected attribute size" }
                var existing = -1
                var insertBefore = count
                for (i in count - 1 downTo 0) {
                    val name = c.getInt(attrStart + i * 20 + 4)
                    if (name == attrName) existing = i
                    val id = if (name in ids.indices) ids[name] else Int.MAX_VALUE
                    if (id.toLong() and 0xffffffffL > ATTR_ANY_DENSITY.toLong()) insertBefore = i
                }
                if (existing >= 0) {
                    val a = attrStart + existing * 20
                    c.putInt(a + 8, -1)
                    c.putInt(a + 12, (TYPE_BOOLEAN shl 24) or 8)
                    c.putInt(a + 16, 0)
                    out.write(chunk)
                } else {
                    val cut = attrStart + insertBefore * 20
                    c.putInt(4, size + 20)
                    c.putShort(28, (count + 1).toShort())
                    out.write(chunk, 0, cut)
                    out.write(attribute())
                    out.write(chunk, cut, size - cut)
                }
                done = true
            } else {
                out.write(chunk)
                if (type == CHUNK_START_ELEMENT && !firstElementSeen) {
                    firstElementSeen = true
                    if (!hasElement) {
                        // No <supports-screens> in the document: add one as the first child of <manifest>.
                        val line = c.getInt(8)
                        out.write(le((0x10 shl 16) or CHUNK_START_ELEMENT, 56, line, -1, -1, elementName, 0x00140014, 1, 0))
                        out.write(attribute())
                        out.write(le((0x10 shl 16) or CHUNK_END_ELEMENT, 24, line, -1, -1, elementName))
                        done = true
                    }
                }
            }
            pos += size
        }
        check(done) { "could not place <supports-screens>" }
        val nodes = out.toByteArray()

        // ---- reassemble ----
        val data = java.io.ByteArrayOutputStream()
        val offsets = IntArray(blobs.size)
        for ((i, b) in blobs.withIndex()) { offsets[i] = data.size(); data.write(b) }
        while (data.size() % 4 != 0) data.write(0)
        val newPoolSize = 28 + blobs.size * 4 + data.size()
        val result = java.io.ByteArrayOutputStream()
        val total = 8 + newPoolSize + 8 + ids.size * 4 + nodes.size
        result.write(le((8 shl 16) or 0x0003, total))
        result.write(le((28 shl 16) or CHUNK_STRING_POOL, newPoolSize, blobs.size, 0, flags and 0x100, 28 + blobs.size * 4, 0))
        result.write(le(*offsets))
        result.write(data.toByteArray())
        result.write(le((8 shl 16) or CHUNK_RESOURCE_MAP, 8 + ids.size * 4))
        result.write(le(*ids.toIntArray()))
        result.write(nodes)
        val patched = result.toByteArray()
        check(isAnyDensityDisabled(patched)) { "manifest edit could not be verified" }
        return patched
    }

    /** True if the document has a <supports-screens> element (texts = its string pool). */
    private fun manifestHasElement(src: ByteBuffer, from: Int, end: Int, texts: List<String>): Boolean {
        var pos = from
        while (pos + 8 <= end) {
            val type = src.getShort(pos).toInt() and 0xffff
            val size = src.getInt(pos + 4)
            if (size < 8) return false
            if (type == CHUNK_START_ELEMENT) {
                val name = src.getInt(pos + 20)
                if (name in texts.indices && texts[name] == "supports-screens") return true
            }
            pos += size
        }
        return false
    }
}
