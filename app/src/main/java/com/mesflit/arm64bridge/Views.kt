// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge

import android.app.Activity
import android.view.View

/** A view of the layout that is known to exist (findViewById is nullable in newer SDKs). */
fun <T : View> Activity.view(id: Int): T = findViewById<T>(id)!!
