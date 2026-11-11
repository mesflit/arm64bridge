// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.content.Context
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.android.apksig.ApkSigner
import java.io.File
import java.io.FilterOutputStream
import java.io.OutputStream
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.Date
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import javax.security.auth.x500.X500Principal

/**
 * Turns a 32-bit-only APK into one that installs and runs on a 64-bit-only device.
 *
 * The patched APK gets a lib/arm64-v8a/ directory containing the bridge runtime
 * (libarm64bridge.so) and, for every original 32-bit library, a tiny 64-bit shim
 * with the same file name. Android therefore treats the app as a normal 64-bit
 * app; when the app loads one of its libraries the shim hands over to the
 * runtime, which loads the untouched 32-bit library from the APK and runs it on
 * an emulated ARMv7 CPU.
 */
class ApkPatcher(private val context: Context) {

    class Analysis(
        val abis: Set<String>,
        val guestAbi: String?,
        val guestLibs: List<String>,
        val importCount: Int,
        val unresolved: List<String>,
        val manifest: AxmlPatcher.Info?,
    ) {
        val minSdk get() = manifest?.minSdk
        val targetSdk get() = manifest?.targetSdk
        val has64 get() = "arm64-v8a" in abis
        /** 32-bit ARM code that this device cannot run: the bridge is added. */
        val needsBridge get() = guestAbi != null && !has64
        /** Every readable APK can be customized (re-signed with the selected changes). */
        val customizable get() = manifest != null
    }

    /** [inGameMenu] only applies to apps that get the bridge (the menu runs inside the bridge runtime). */
    class Options(val manifest: AxmlPatcher.Edits, val inGameMenu: Boolean = false)

    fun analyze(apk: File): Analysis {
        ZipFile(apk).use { zip ->
            val libs = zip.entries().asSequence()
                .filter { !it.isDirectory && it.name.startsWith("lib/") && it.name.endsWith(".so") }
                .filter { it.name.count { c -> c == '/' } == 2 }
                .toList()
            val abis = libs.map { it.name.split('/')[1] }.toSortedSet()
            val guestAbi = listOf("armeabi-v7a", "armeabi").firstOrNull { it in abis }
            val guestEntries = if (guestAbi == null) emptyList() else libs.filter { it.name.startsWith("lib/$guestAbi/") }

            var imports = 0
            val unresolved = sortedSetOf<String>()
            val exported = HashSet<String>()
            for (entry in guestEntries) {
                val bytes = zip.getInputStream(entry).use { it.readBytes() }
                for (line in NativeBridge.inspectElf(bytes).lineSequence()) {
                    val eq = line.indexOf('=')
                    if (eq < 0) continue
                    val value = line.substring(eq + 1)
                    when (line.substring(0, eq)) {
                        "imports" -> imports += value.toIntOrNull() ?: 0
                        "unresolved" -> unresolved.add(value)
                        "export" -> exported.add(value)
                    }
                }
            }
            // Functions that another bundled library provides are not missing.
            unresolved.removeAll(exported)

            val manifest = zip.getEntry("AndroidManifest.xml")?.let { e -> zip.getInputStream(e).use { it.readBytes() } }
            val info = manifest?.let { runCatching { AxmlPatcher.info(it) }.getOrNull() }
            return Analysis(
                abis, guestAbi, guestEntries.map { it.name.substringAfterLast('/') },
                imports, unresolved.toList(), info
            )
        }
    }

    /** Writes the patched and signed APK to [output]. [progress] receives values from 0 to 100. */
    fun patch(input: File, output: File, options: Options, progress: (Int) -> Unit) {
        val analysis = analyze(input)
        check(analysis.customizable) { "this file has no readable AndroidManifest.xml" }
        val bridge = analysis.needsBridge
        val nativeDir = File(context.applicationInfo.nativeLibraryDir)
        val runtime = File(nativeDir, "libarm64bridge.so")
        val shim = File(nativeDir, "liba64bshim.so")
        if (bridge) check(runtime.isFile && shim.isFile) { "bridge runtime libraries are missing from $nativeDir" }
        val shimBytes = if (bridge) shim.readBytes() else ByteArray(0)

        val unsigned = File(output.parentFile, output.name + ".unsigned")
        try {
            ZipFile(input).use { zip ->
                val counter = CountingStream(unsigned.outputStream().buffered(1 shl 16))
                ZipOutputStream(counter).use { out ->
                    val seen = HashSet<String>()
                    val entries = zip.entries().toList()
                    for ((index, entry) in entries.withIndex()) {
                        progress(index * 80 / entries.size.coerceAtLeast(1))
                        val name = entry.name
                        if (entry.isDirectory || !seen.add(name)) continue
                        if (isSignatureFile(name) || name.startsWith(MENU_ASSETS)) continue
                        if (bridge && name.startsWith("lib/arm64-v8a/")) continue
                        if (name == "AndroidManifest.xml" && !options.manifest.isEmpty) {
                            val manifest = zip.getInputStream(entry).use { it.readBytes() }
                            writeDeflated(out, name, AxmlPatcher.apply(manifest, options.manifest))
                            continue
                        }
                        if (entry.method == ZipEntry.STORED) {
                            val bytesOrNull = if (entry.size <= MAX_IN_MEMORY) zip.getInputStream(entry).use { it.readBytes() } else null
                            val e = ZipEntry(name).apply {
                                method = ZipEntry.STORED
                                size = entry.size
                                compressedSize = entry.size
                                crc = entry.crc
                                time = FIXED_TIME
                            }
                            align(e, counter, out, 4)
                            out.putNextEntry(e)
                            if (bytesOrNull != null) out.write(bytesOrNull) else zip.getInputStream(entry).use { it.copyTo(out, 1 shl 16) }
                            out.closeEntry()
                        } else {
                            val e = ZipEntry(name).apply { time = FIXED_TIME }
                            out.putNextEntry(e)
                            zip.getInputStream(entry).use { it.copyTo(out, 1 shl 16) }
                            out.closeEntry()
                        }
                    }
                    if (bridge && options.inGameMenu) {
                        // The in-game menu is a class of this app; the runtime loads it from these copies of our dex.
                        ZipFile(context.applicationInfo.sourceDir).use { self ->
                            for (dex in self.entries().asSequence().filter { it.name.matches(Regex("classes\\d*\\.dex")) }) {
                                writeDeflated(out, MENU_ASSETS + dex.name, self.getInputStream(dex).use { it.readBytes() })
                            }
                        }
                    }
                    if (bridge) {
                        // The bridge runtime plus one shim per original library.
                        writeStoredAligned(out, counter, "lib/arm64-v8a/libarm64bridge.so", runtime.readBytes())
                        for (lib in analysis.guestLibs) {
                            if (lib == "libarm64bridge.so") continue
                            writeStoredAligned(out, counter, "lib/arm64-v8a/$lib", shimBytes)
                        }
                    }
                }
            }
            progress(85)
            sign(unsigned, output)
            progress(100)
        } finally {
            unsigned.delete()
        }
    }

    private fun isSignatureFile(name: String): Boolean {
        if (!name.startsWith("META-INF/")) return false
        val upper = name.uppercase()
        return upper == "META-INF/MANIFEST.MF" || upper.endsWith(".SF") || upper.endsWith(".RSA") ||
            upper.endsWith(".DSA") || upper.endsWith(".EC")
    }

    private fun writeDeflated(out: ZipOutputStream, name: String, data: ByteArray) {
        out.putNextEntry(ZipEntry(name).apply { time = FIXED_TIME })
        out.write(data)
        out.closeEntry()
    }

    /** Uncompressed and page aligned, so the library also works when the app maps it straight from the APK. */
    private fun writeStoredAligned(out: ZipOutputStream, counter: CountingStream, name: String, data: ByteArray) {
        val crc = java.util.zip.CRC32().apply { update(data) }
        val e = ZipEntry(name).apply {
            method = ZipEntry.STORED
            size = data.size.toLong()
            compressedSize = data.size.toLong()
            this.crc = crc.value
            time = FIXED_TIME
        }
        align(e, counter, out, 16384)
        out.putNextEntry(e)
        out.write(data)
        out.closeEntry()
    }

    /** Pads the local header with an extra field so that the entry data starts at a multiple of [alignment]. */
    private fun align(entry: ZipEntry, counter: CountingStream, out: ZipOutputStream, alignment: Int) {
        out.flush()
        val dataStart = counter.count + 30 + entry.name.toByteArray(Charsets.UTF_8).size
        val pad = ((alignment - (dataStart % alignment)) % alignment).toInt()
        if (pad > 0) entry.extra = ByteArray(pad)
    }

    private fun sign(unsigned: File, output: File) {
        val (key, cert) = signingKey()
        val signer = ApkSigner.SignerConfig.Builder("ARM64BRIDGE", key, listOf(cert)).build()
        ApkSigner.Builder(listOf(signer))
            .setInputApk(unsigned)
            .setOutputApk(output)
            .setV1SigningEnabled(true)
            .setV2SigningEnabled(true)
            .setV3SigningEnabled(false)
            .setV4SigningEnabled(false)
            .setMinSdkVersion(24)
            .setCreatedBy("ARM64Bridge")
            .build()
            .sign()
    }

    /** A device-local key in the Android Keystore; every app patched on this device is signed with it. */
    private fun signingKey(): Pair<PrivateKey, X509Certificate> {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (!ks.containsAlias(KEY_ALIAS)) {
            val spec = KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_SIGN)
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512, KeyProperties.DIGEST_SHA1)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setCertificateSubject(X500Principal("CN=ARM64Bridge patched app"))
                .setCertificateSerialNumber(BigInteger.ONE)
                .setCertificateNotBefore(Date(System.currentTimeMillis() - 86_400_000L))
                .setCertificateNotAfter(Date(System.currentTimeMillis() + 30L * 365 * 86_400_000L))
                .build()
            KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore").apply { initialize(spec) }
                .generateKeyPair()
        }
        return Pair(ks.getKey(KEY_ALIAS, null) as PrivateKey, ks.getCertificate(KEY_ALIAS) as X509Certificate)
    }

    private class CountingStream(out: OutputStream) : FilterOutputStream(out) {
        var count = 0L
        override fun write(b: Int) {
            out.write(b)
            count++
        }

        override fun write(b: ByteArray, off: Int, len: Int) {
            out.write(b, off, len)
            count += len
        }
    }

    companion object {
        private const val KEY_ALIAS = "arm64bridge-apk-signing"
        private const val MENU_ASSETS = "assets/arm64bridge/"
        private const val MAX_IN_MEMORY = 8L * 1024 * 1024
        private const val FIXED_TIME = 1230768000000L // 2009-01-01, keeps output reproducible

        /** Lowest targetSdkVersion this device still installs (0 if there is no restriction). */
        fun minInstallableTargetSdk(): Int = when {
            Build.VERSION.SDK_INT >= 35 -> 24
            Build.VERSION.SDK_INT >= 34 -> 23
            else -> 0
        }
    }
}
