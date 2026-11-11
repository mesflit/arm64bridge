// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Editable model of a compiled (binary) Android XML document such as AndroidManifest.xml.
 * Strings are kept as text, so nodes and attributes can be added, removed and changed freely;
 * [toBytes] rebuilds the string pool and resource map.
 */
class Axml private constructor(private val utf8: Boolean, val nodes: MutableList<Node>) {

    sealed class Node { var line: Int = 0 }
    class StartNs(var prefix: String?, var uri: String?) : Node()
    class EndNs(var prefix: String?, var uri: String?) : Node()
    class Element(var ns: String?, var name: String, val attrs: MutableList<Attr> = ArrayList()) : Node() {
        var idAttr: Attr? = null
        var classAttr: Attr? = null
        var styleAttr: Attr? = null

        fun attr(resId: Int): Attr? = attrs.firstOrNull { it.resId == resId }
        fun attr(name: String): Attr? = attrs.firstOrNull { it.resId == 0 && it.name == name }

        /** Adds or replaces an android: attribute, keeping attributes ordered by resource ID. */
        fun set(resId: Int, name: String, type: Int, data: Int, str: String? = null): Attr {
            attrs.firstOrNull { it.resId == resId }?.let {
                it.type = type; it.data = data; it.str = str; it.raw = if (type == TYPE_STRING) str else null
                return it
            }
            val a = Attr(ANDROID_NS, name, resId, if (type == TYPE_STRING) str else null, type, data, str)
            val at = attrs.indexOfFirst { it.resId == 0 || (it.resId.toLong() and 0xffffffffL) > (resId.toLong() and 0xffffffffL) }
            if (at < 0) attrs.add(a) else attrs.add(at, a)
            return a
        }

        fun setString(resId: Int, name: String, value: String) = set(resId, name, TYPE_STRING, 0, value)
        fun setBoolean(resId: Int, name: String, value: Boolean) = set(resId, name, TYPE_BOOLEAN, if (value) -1 else 0)
        fun setInt(resId: Int, name: String, value: Int) = set(resId, name, TYPE_INT_DEC, value)
        fun string(resId: Int): String? = attr(resId)?.takeIf { it.type == TYPE_STRING }?.str
    }
    class EndElement(var ns: String?, var name: String) : Node()
    class Cdata(var text: String?, var type: Int, var data: Int, var str: String?) : Node()

    class Attr(var ns: String?, var name: String, var resId: Int, var raw: String?, var type: Int, var data: Int, var str: String?)

    /** Elements in document order, with their depth (the root element has depth 1). */
    fun elements(): List<Pair<Element, Int>> {
        val out = ArrayList<Pair<Element, Int>>()
        var depth = 0
        for (n in nodes) when (n) {
            is Element -> { depth++; out.add(n to depth) }
            is EndElement -> depth--
            else -> {}
        }
        return out
    }

    fun root(): Element = nodes.first { it is Element } as Element

    /** Children of [parent] (direct only) with the given name. */
    fun children(parent: Element, name: String? = null): List<Element> {
        val out = ArrayList<Element>()
        var i = nodes.indexOf(parent) + 1
        var depth = 0
        while (i < nodes.size) {
            when (val n = nodes[i]) {
                is Element -> { if (depth == 0 && (name == null || n.name == name)) out.add(n); depth++ }
                is EndElement -> { if (depth == 0) break; depth-- }
                else -> {}
            }
            i++
        }
        return out
    }

    /** Removes an element together with everything inside it. */
    fun remove(element: Element) {
        val start = nodes.indexOf(element)
        var depth = 0
        var i = start
        while (i < nodes.size) {
            when (nodes[i]) {
                is Element -> depth++
                is EndElement -> { depth--; if (depth == 0) break }
                else -> {}
            }
            i++
        }
        nodes.subList(start, i + 1).clear()
    }

    /** Inserts a new empty element as a child of [parent], after [after] (or as the first child). */
    fun insertChild(parent: Element, name: String, after: Element? = null): Element {
        val e = Element(null, name)
        e.line = parent.line
        val end = EndElement(null, name)
        end.line = parent.line
        val at = if (after == null) nodes.indexOf(parent) + 1 else endIndex(after) + 1
        nodes.add(at, end)
        nodes.add(at, e)
        return e
    }

    private fun endIndex(element: Element): Int {
        var depth = 0
        for (i in nodes.indexOf(element) until nodes.size) {
            when (nodes[i]) {
                is Element -> depth++
                is EndElement -> { depth--; if (depth == 0) return i }
                else -> {}
            }
        }
        error("unbalanced document")
    }

    fun toBytes(): ByteArray {
        // String pool: attribute names that carry a resource ID come first, in the same order as
        // the resource map; then every other string.
        val pool = ArrayList<String>()
        val ids = ArrayList<Int>()
        val idIndex = HashMap<Pair<String, Int>, Int>()
        val plainIndex = HashMap<String, Int>()
        for (n in nodes) if (n is Element) for (a in n.attrs) if (a.resId != 0) {
            idIndex.getOrPut(a.name to a.resId) { pool.add(a.name); ids.add(a.resId); pool.size - 1 }
        }
        fun idx(s: String?): Int = if (s == null) -1 else plainIndex.getOrPut(s) { pool.add(s); pool.size - 1 }
        fun nameIdx(a: Attr) = if (a.resId != 0) idIndex.getValue(a.name to a.resId) else idx(a.name)

        val body = ByteArrayOutputStream()
        fun chunk(type: Int, line: Int, ext: ByteArray) {
            body.write(le16(type, 16)); body.write(le(16 + ext.size, line, -1)); body.write(ext)
        }
        for (n in nodes) when (n) {
            is StartNs -> chunk(CHUNK_START_NS, n.line, le(idx(n.prefix), idx(n.uri)))
            is EndNs -> chunk(CHUNK_END_NS, n.line, le(idx(n.prefix), idx(n.uri)))
            is EndElement -> chunk(CHUNK_END_ELEMENT, n.line, le(idx(n.ns), idx(n.name)))
            is Cdata -> {
                val ext = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
                ext.putInt(idx(n.text)).putShort(8).put(0).put(n.type.toByte()).putInt(if (n.type == TYPE_STRING) idx(n.str) else n.data)
                chunk(CHUNK_CDATA, n.line, ext.array())
            }
            is Element -> {
                val ext = ByteBuffer.allocate(20 + 20 * n.attrs.size).order(ByteOrder.LITTLE_ENDIAN)
                ext.putInt(idx(n.ns)).putInt(idx(n.name)).putShort(20).putShort(20).putShort(n.attrs.size.toShort())
                fun pos(a: Attr?) = if (a == null) 0 else n.attrs.indexOf(a) + 1
                ext.putShort(pos(n.idAttr).toShort()).putShort(pos(n.classAttr).toShort()).putShort(pos(n.styleAttr).toShort())
                for (a in n.attrs) {
                    ext.putInt(idx(a.ns)).putInt(nameIdx(a)).putInt(idx(a.raw))
                    ext.putShort(8).put(0).put(a.type.toByte())
                    ext.putInt(if (a.type == TYPE_STRING) idx(a.str) else a.data)
                }
                chunk(CHUNK_START_ELEMENT, n.line, ext.array())
            }
        }

        val strings = ByteArrayOutputStream()
        val offsets = IntArray(pool.size)
        for ((i, s) in pool.withIndex()) {
            offsets[i] = strings.size()
            if (utf8) {
                val b = s.toByteArray(Charsets.UTF_8)
                writeLen8(strings, s.length); writeLen8(strings, b.size); strings.write(b); strings.write(0)
            } else {
                if (s.length > 0x7fff) {
                    strings.write(le16(0x8000 or (s.length ushr 16), s.length and 0xffff))
                } else {
                    strings.write(le16short(s.length))
                }
                strings.write(s.toByteArray(Charsets.UTF_16LE)); strings.write(byteArrayOf(0, 0))
            }
        }
        while (strings.size() % 4 != 0) strings.write(0)
        val poolSize = 28 + 4 * pool.size + strings.size()
        val mapSize = 8 + 4 * ids.size
        val out = ByteArrayOutputStream()
        out.write(le16(0x0003, 8)); out.write(le(8 + poolSize + mapSize + body.size()))
        out.write(le16(CHUNK_STRING_POOL, 28)); out.write(le(poolSize, pool.size, 0, if (utf8) 0x100 else 0, 28 + 4 * pool.size, 0))
        out.write(le(*offsets)); out.write(strings.toByteArray())
        out.write(le16(CHUNK_RESOURCE_MAP, 8)); out.write(le(mapSize)); out.write(le(*ids.toIntArray()))
        out.write(body.toByteArray())
        return out.toByteArray()
    }

    companion object {
        const val ANDROID_NS = "http://schemas.android.com/apk/res/android"
        const val TYPE_REFERENCE = 0x01
        const val TYPE_STRING = 0x03
        const val TYPE_INT_DEC = 0x10
        const val TYPE_INT_HEX = 0x11
        const val TYPE_BOOLEAN = 0x12

        private const val CHUNK_STRING_POOL = 0x0001
        private const val CHUNK_RESOURCE_MAP = 0x0180
        private const val CHUNK_START_NS = 0x0100
        private const val CHUNK_END_NS = 0x0101
        private const val CHUNK_START_ELEMENT = 0x0102
        private const val CHUNK_END_ELEMENT = 0x0103
        private const val CHUNK_CDATA = 0x0104

        fun parse(data: ByteArray): Axml {
            val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
            require(data.size >= 8 && (b.getShort(0).toInt() and 0xffff) == 0x0003) { "not a binary XML document" }
            var pos = b.getShort(2).toInt() and 0xffff
            val end = minOf(b.getInt(4).let { if (it in 8..data.size) it else data.size }, data.size)
            var strings: List<String> = emptyList()
            var utf8 = false
            var resIds = IntArray(0)
            val nodes = ArrayList<Node>()
            fun s(i: Int): String? = if (i < 0 || i >= strings.size) null else strings[i]
            while (pos + 8 <= end) {
                val type = b.getShort(pos).toInt() and 0xffff
                val header = b.getShort(pos + 2).toInt() and 0xffff
                val size = b.getInt(pos + 4)
                require(size >= 8 && pos + size <= end) { "corrupt chunk at $pos" }
                when (type) {
                    CHUNK_STRING_POOL -> {
                        val count = b.getInt(pos + 8)
                        require(b.getInt(pos + 12) == 0) { "styled strings are not supported" }
                        utf8 = b.getInt(pos + 16) and 0x100 != 0
                        val base = pos + b.getInt(pos + 20)
                        strings = List(count) { readString(b, base + b.getInt(pos + header + it * 4), utf8) }
                    }
                    CHUNK_RESOURCE_MAP -> resIds = IntArray((size - header) / 4) { b.getInt(pos + header + it * 4) }
                    CHUNK_START_NS, CHUNK_END_NS -> {
                        val prefix = s(b.getInt(pos + 16)); val uri = s(b.getInt(pos + 20))
                        nodes.add((if (type == CHUNK_START_NS) StartNs(prefix, uri) else EndNs(prefix, uri)).also { it.line = b.getInt(pos + 8) })
                    }
                    CHUNK_START_ELEMENT -> {
                        val ext = pos + header
                        val e = Element(s(b.getInt(ext)), s(b.getInt(ext + 4)) ?: "")
                        e.line = b.getInt(pos + 8)
                        val attrStart = b.getShort(ext + 8).toInt() and 0xffff
                        val attrSize = b.getShort(ext + 10).toInt() and 0xffff
                        val count = b.getShort(ext + 12).toInt() and 0xffff
                        for (i in 0 until count) {
                            val a = ext + attrStart + i * attrSize
                            val nameIdx = b.getInt(a + 4)
                            val t = b.get(a + 15).toInt() and 0xff
                            val d = b.getInt(a + 16)
                            e.attrs.add(Attr(s(b.getInt(a)), s(nameIdx) ?: "", if (nameIdx in resIds.indices) resIds[nameIdx] else 0,
                                s(b.getInt(a + 8)), t, d, if (t == TYPE_STRING) s(d) else null))
                        }
                        fun byPos(p: Int) = if (p in 1..e.attrs.size) e.attrs[p - 1] else null
                        e.idAttr = byPos(b.getShort(ext + 14).toInt() and 0xffff)
                        e.classAttr = byPos(b.getShort(ext + 16).toInt() and 0xffff)
                        e.styleAttr = byPos(b.getShort(ext + 18).toInt() and 0xffff)
                        nodes.add(e)
                    }
                    CHUNK_END_ELEMENT -> nodes.add(EndElement(s(b.getInt(pos + 16)), s(b.getInt(pos + 20)) ?: "").also { it.line = b.getInt(pos + 8) })
                    CHUNK_CDATA -> {
                        val t = b.get(pos + 23).toInt() and 0xff
                        val d = b.getInt(pos + 24)
                        nodes.add(Cdata(s(b.getInt(pos + 16)), t, d, if (t == TYPE_STRING) s(d) else null).also { it.line = b.getInt(pos + 8) })
                    }
                    else -> error("unsupported chunk type 0x${type.toString(16)}")
                }
                pos += size
            }
            return Axml(utf8, nodes)
        }

        private fun readString(b: ByteBuffer, start: Int, utf8: Boolean): String {
            var p = start
            return if (utf8) {
                p += if (b.get(p).toInt() and 0x80 != 0) 2 else 1
                var n = b.get(p).toInt() and 0xff
                if (n and 0x80 != 0) { n = ((n and 0x7f) shl 8) or (b.get(p + 1).toInt() and 0xff); p += 2 } else p += 1
                String(b.array(), p, n, Charsets.UTF_8)
            } else {
                var n = b.getShort(p).toInt() and 0xffff
                p += 2
                if (n and 0x8000 != 0) { n = ((n and 0x7fff) shl 16) or (b.getShort(p).toInt() and 0xffff); p += 2 }
                String(b.array(), p, n * 2, Charsets.UTF_16LE)
            }
        }

        private fun writeLen8(out: ByteArrayOutputStream, n: Int) {
            require(n < 0x8000) { "string too long" }
            if (n > 0x7f) { out.write(0x80 or (n ushr 8)); out.write(n and 0xff) } else out.write(n)
        }

        private fun le(vararg v: Int): ByteArray {
            val b = ByteBuffer.allocate(v.size * 4).order(ByteOrder.LITTLE_ENDIAN)
            v.forEach { b.putInt(it) }
            return b.array()
        }
        private fun le16(a: Int, c: Int): ByteArray =
            ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putShort(a.toShort()).putShort(c.toShort()).array()
        private fun le16short(a: Int): ByteArray = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN).putShort(a.toShort()).array()
    }
}
