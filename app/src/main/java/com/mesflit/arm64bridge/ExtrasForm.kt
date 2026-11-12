// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.view.View
import android.widget.Button
import android.widget.TextView
import java.io.FilterInputStream
import java.util.zip.ZipInputStream

/**
 * The "OBB and data files" part of the main screen: OBB files, a data ZIP or a data folder that the
 * patcher puts into the APK (see [ApkPatcher.Extra]). The pickers return through
 * [Activity.onActivityResult], which hands the result to [onResult].
 */
class ExtrasForm(private val activity: Activity) {
    private val list: TextView = activity.view(R.id.extrasList)
    private val clear: Button = activity.view(R.id.clearExtras)
    /** Description shown to the user, and the patcher input. */
    private val extras = ArrayList<Pair<String, ApkPatcher.Extra>>()

    val chosen: List<ApkPatcher.Extra> get() = extras.map { it.second }

    init {
        clear.setOnClickListener { reset() }
        activity.view<Button>(R.id.addObb).setOnClickListener {
            pick(REQUEST_OBB, Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            })
        }
        activity.view<Button>(R.id.addDataZip).setOnClickListener {
            pick(REQUEST_DATA_ZIP, Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
            })
        }
        activity.view<Button>(R.id.addDataFolder).setOnClickListener {
            pick(REQUEST_DATA_FOLDER, Intent(Intent.ACTION_OPEN_DOCUMENT_TREE))
        }
        show()
    }

    fun reset() {
        extras.clear()
        show()
    }

    /** Takes a picker result; false if [requestCode] is not one of ours. */
    fun onResult(requestCode: Int, data: Intent): Boolean {
        when (requestCode) {
            REQUEST_OBB -> {
                val uris = data.clipData?.let { c -> (0 until c.itemCount).map { c.getItemAt(it).uri } } ?: listOfNotNull(data.data)
                uris.forEach { addObb(it) }
            }
            REQUEST_DATA_ZIP -> data.data?.let { addDataZip(it) }
            REQUEST_DATA_FOLDER -> data.data?.let { addDataFolder(it) }
            else -> return false
        }
        show()
        return true
    }

    @Suppress("DEPRECATION")
    private fun pick(requestCode: Int, intent: Intent) = activity.startActivityForResult(intent, requestCode)

    private fun show() {
        list.text = if (extras.isEmpty()) activity.getString(R.string.extras_none) else extras.joinToString("\n") { it.first }
        clear.visibility = if (extras.isEmpty()) View.GONE else View.VISIBLE
    }

    private fun addObb(uri: Uri) {
        val resolver = activity.contentResolver
        val name = resolver.displayName(uri) ?: return
        extras += activity.getString(R.string.extras_obb, name) to ApkPatcher.Extra(ApkPatcher.Extra.Kind.OBB) { emit ->
            resolver.openInputStream(uri)!!.use { emit(name, it) }
        }
    }

    private fun addDataZip(uri: Uri) {
        val resolver = activity.contentResolver
        val name = resolver.displayName(uri) ?: "data.zip"
        extras += activity.getString(R.string.extras_data_zip, name) to ApkPatcher.Extra(ApkPatcher.Extra.Kind.DATA) { emit ->
            ZipInputStream(resolver.openInputStream(uri)!!.buffered(1 shl 16)).use { zip ->
                val entryStream = object : FilterInputStream(zip) { override fun close() {} }
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (!entry.isDirectory) emit(entry.name, entryStream)
                }
            }
        }
    }

    private fun addDataFolder(tree: Uri) {
        val resolver = activity.contentResolver
        val root = DocumentsContract.getTreeDocumentId(tree)
        val name = root.substringAfterLast('/').substringAfterLast(':')
        extras += activity.getString(R.string.extras_data_folder, name) to ApkPatcher.Extra(ApkPatcher.Extra.Kind.DATA) { emit ->
            fun walk(docId: String, prefix: String) {
                val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
                val cols = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE)
                val items = ArrayList<Triple<String, String, Boolean>>()
                resolver.query(children, cols, null, null, null)?.use { c ->
                    while (c.moveToNext()) items += Triple(c.getString(0), c.getString(1), c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR)
                }
                for ((id, child, isDir) in items) {
                    if (isDir) walk(id, "$prefix$child/")
                    else resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id))!!.use { emit(prefix + child, it) }
                }
            }
            // The chosen folder is the package folder (or a folder inside it, such as "files").
            walk(root, if (name == "files" || name == "cache") "$name/" else "")
        }
    }

    companion object {
        const val REQUEST_OBB = 103
        const val REQUEST_DATA_ZIP = 104
        const val REQUEST_DATA_FOLDER = 105
    }
}
