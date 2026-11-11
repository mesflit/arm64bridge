// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.app.Activity;
import android.app.Application;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * Floating in-game menu. This class is not used by the ARM64Bridge app itself: when the "in-game menu"
 * option is chosen, the patcher copies the app's dex into the patched APK and the bridge runtime loads
 * this class inside the game's process. It only uses framework classes.
 *
 * The menu lives inside the game's own window (no overlay permission) and controls bridge settings.
 */
public final class GameOverlay implements Application.ActivityLifecycleCallbacks {
    private static final String TAG_VIEW = "arm64bridge-overlay";
    private static final String PREFS = "arm64bridge";
    private static final String[] LIMITER_LABELS = {"Frame limiter: game default", "Frame limiter: relaxed (×2)", "Frame limiter: off"};

    /** 0 = leave the game's frame limiter alone, 1 = halve its waits, 2 = skip its waits. */
    private static native void nativeSetLimiter(int mode);
    /** Frames per second drawn through the bridge since the previous call, or a negative value if unknown. */
    private static native float nativeFps();

    private final Handler handler = new Handler(Looper.getMainLooper());
    private int limiter;

    /** Called by the bridge runtime once the class is loaded. */
    public static void install(Application app) {
        GameOverlay overlay = new GameOverlay();
        overlay.limiter = app.getSharedPreferences(PREFS, 0).getInt("limiter", 0);
        nativeSetLimiter(overlay.limiter);
        app.registerActivityLifecycleCallbacks(overlay);
    }

    private void attach(final Activity activity) {
        final View decor = activity.getWindow().getDecorView();
        if (decor.findViewWithTag(TAG_VIEW) != null) return;
        final float dp = activity.getResources().getDisplayMetrics().density;

        final LinearLayout root = new LinearLayout(activity);
        root.setTag(TAG_VIEW);
        root.setOrientation(LinearLayout.VERTICAL);

        final TextView handle = label(activity, "A64B", dp);
        final LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setVisibility(View.GONE);
        final TextView fps = label(activity, "FPS: –", dp);
        final TextView limiterButton = label(activity, LIMITER_LABELS[limiter], dp);
        final TextView note = label(activity, "If the game runs too fast with the limiter off,\nits speed is tied to the frame rate: switch back.", dp);
        note.setTextSize(10);
        panel.addView(fps);
        panel.addView(limiterButton);
        panel.addView(note);
        root.addView(handle);
        root.addView(panel);

        limiterButton.setOnClickListener(v -> {
            limiter = (limiter + 1) % LIMITER_LABELS.length;
            limiterButton.setText(LIMITER_LABELS[limiter]);
            nativeSetLimiter(limiter);
            SharedPreferences.Editor e = activity.getSharedPreferences(PREFS, 0).edit();
            e.putInt("limiter", limiter);
            e.apply();
        });

        // Drag the handle to move the menu, tap it to open or close the panel.
        final int slop = ViewConfiguration.get(activity).getScaledTouchSlop();
        handle.setOnTouchListener(new View.OnTouchListener() {
            float downX, downY, startX, startY;
            boolean moved;

            @Override
            public boolean onTouch(View v, MotionEvent ev) {
                switch (ev.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX = ev.getRawX();
                        downY = ev.getRawY();
                        startX = root.getTranslationX();
                        startY = root.getTranslationY();
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = ev.getRawX() - downX, dy = ev.getRawY() - downY;
                        if (Math.abs(dx) > slop || Math.abs(dy) > slop) moved = true;
                        if (moved) {
                            root.setTranslationX(startX + dx);
                            root.setTranslationY(startY + dy);
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (!moved) panel.setVisibility(panel.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
                        return true;
                    default:
                        return false;
                }
            }
        });

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START);
        lp.topMargin = (int) (48 * dp);
        lp.leftMargin = (int) (8 * dp);
        activity.addContentView(root, lp);

        handler.post(new Runnable() {
            @Override
            public void run() {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                float value = nativeFps();
                String text = value < 0 ? "FPS: –" : String.format(Locale.US, "FPS: %.0f", value);
                fps.setText(text);
                handle.setText(panel.getVisibility() == View.VISIBLE || value < 0 ? "A64B" : String.format(Locale.US, "%.0f", value));
                handler.postDelayed(this, 1000);
            }
        });
    }

    private static TextView label(Activity activity, String text, float dp) {
        TextView t = new TextView(activity);
        t.setText(text);
        t.setTextColor(Color.WHITE);
        t.setTextSize(13);
        int pad = (int) (8 * dp);
        t.setPadding(pad, pad / 2, pad, pad / 2);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xB0000000);
        bg.setCornerRadius(6 * dp);
        t.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = (int) (2 * dp);
        t.setLayoutParams(lp);
        return t;
    }

    @Override public void onActivityResumed(Activity activity) {
        try {
            attach(activity);
        } catch (RuntimeException ignored) {
            // Never let the menu break the game.
        }
    }
    @Override public void onActivityCreated(Activity activity, Bundle state) {}
    @Override public void onActivityStarted(Activity activity) {}
    @Override public void onActivityPaused(Activity activity) {}
    @Override public void onActivityStopped(Activity activity) {}
    @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
    @Override public void onActivityDestroyed(Activity activity) {}

    private GameOverlay() {}
}
