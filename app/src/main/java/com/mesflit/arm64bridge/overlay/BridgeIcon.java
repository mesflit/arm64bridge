// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;

/** The ARM64Bridge launcher icon, drawn in code: inside a patched game the app's resources are not available. */
final class BridgeIcon extends Drawable {
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arch = new Path();
    private final RectF rect = new RectF();

    @Override
    public void draw(Canvas canvas) {
        final float s = Math.min(getBounds().width(), getBounds().height()) / 48f;  // the icon is designed on a 48x48 grid
        canvas.save();
        canvas.translate(getBounds().left, getBounds().top);
        canvas.scale(s, s);

        fill.setColor(0xFF1B5E20);
        rect.set(0, 0, 48, 48);
        canvas.drawRoundRect(rect, 8, 8, fill);

        fill.setColor(0xFFFFFFFF);
        canvas.drawRect(6, 28, 42, 32, fill);          // deck
        canvas.drawRect(10, 32, 13, 40, fill);         // pillars
        canvas.drawRect(35, 32, 38, 40, fill);
        canvas.drawRect(15, 20, 17, 28, fill);         // hangers
        canvas.drawRect(23, 16, 25, 28, fill);
        canvas.drawRect(31, 20, 33, 28, fill);

        stroke.setColor(0xFFFFFFFF);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3);
        arch.reset();
        arch.moveTo(8, 28);
        arch.cubicTo(14, 12, 34, 12, 40, 28);
        canvas.drawPath(arch, stroke);
        canvas.restore();
    }

    @Override public void setAlpha(int alpha) { fill.setAlpha(alpha); stroke.setAlpha(alpha); }
    @Override public void setColorFilter(ColorFilter filter) { fill.setColorFilter(filter); stroke.setColorFilter(filter); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}
