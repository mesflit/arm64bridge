// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.FrameLayout;
import android.widget.ImageView;
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
    private static final String[] LIMITER_LABELS = {"FPS booster: off (game limit)", "FPS booster: 60 FPS", "FPS booster: unlimited"};
    private static final String PROJECT_URL = "https://github.com/mesflit/arm64bridge";

    /** 0 = leave the game's frame limiter alone, 1 = 60 FPS, 2 = no limit. */
    private static native void nativeSetLimiter(int mode);
    /** Frames per second drawn through the bridge since the previous call, or a negative value if unknown. */
    private static native float nativeFps();

    private final Handler handler = new Handler(Looper.getMainLooper());
    private int limiter;
    private boolean booster;

    /**
     * Called by the bridge runtime once the class is loaded. The frame rate is always shown;
     * [booster] (an option of the patcher) adds the FPS booster controls.
     */
    public static void install(Application app, boolean booster) {
        GameOverlay overlay = new GameOverlay();
        overlay.booster = booster;
        overlay.limiter = booster ? app.getSharedPreferences(PREFS, 0).getInt("limiter", 0) : 0;
        if (overlay.limiter < 0 || overlay.limiter >= LIMITER_LABELS.length) overlay.limiter = 0;
        nativeSetLimiter(overlay.limiter);
        app.registerActivityLifecycleCallbacks(overlay);
    }

    private void attach(final Activity activity) {
        final View decor = activity.getWindow().getDecorView();
        if (decor.findViewWithTag(TAG_VIEW) != null) return;
        final float dp = activity.getResources().getDisplayMetrics().density;

        // A fixed-width column: the icon (always visible) and the panel below it. The column itself is
        // transparent and not clickable, so touches outside its children go to the game.
        final LinearLayout root = new LinearLayout(activity);
        root.setTag(TAG_VIEW);
        root.setOrientation(LinearLayout.VERTICAL);

        final LinearLayout handleRow = new LinearLayout(activity);
        handleRow.setOrientation(LinearLayout.HORIZONTAL);
        handleRow.setGravity(Gravity.CENTER_VERTICAL);
        final ImageView handle = new ImageView(activity);
        handle.setImageDrawable(new BridgeIcon());
        handle.setAlpha(0.85f);
        final int iconSize = (int) (40 * dp);
        handleRow.addView(handle, new LinearLayout.LayoutParams(iconSize, iconSize));
        final TextView badge = label(activity, "", dp);
        badge.setVisibility(View.GONE);
        LinearLayout.LayoutParams badgeLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        badgeLp.leftMargin = (int) (4 * dp);
        handleRow.addView(badge, badgeLp);

        final LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setVisibility(View.GONE);
        final TextView fps = label(activity, "FPS: –", dp);
        final TextView limiterButton = label(activity, LIMITER_LABELS[limiter], dp);
        final TextView note = label(activity, "Raises the game's own frame limit. If the game runs too fast or stutters, switch back to off.", dp);
        note.setTextSize(10);
        final TextView about = label(activity, "ARM64Bridge · hold the icon to open the project page", dp);
        about.setTextSize(10);
        LinearLayout.LayoutParams row = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        row.topMargin = (int) (3 * dp);
        panel.addView(fps, row);
        if (booster) {
            panel.addView(limiterButton, row);
            panel.addView(note, row);
        }
        panel.addView(about, row);
        root.addView(handleRow, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(panel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        limiterButton.setOnClickListener(v -> {
            limiter = (limiter + 1) % LIMITER_LABELS.length;
            limiterButton.setText(LIMITER_LABELS[limiter]);
            nativeSetLimiter(limiter);
            SharedPreferences.Editor e = activity.getSharedPreferences(PREFS, 0).edit();
            e.putInt("limiter", limiter);
            e.apply();
        });

        // Drag the handle to move the menu, tap it to open or close the panel, hold it to open the project page.
        final int slop = ViewConfiguration.get(activity).getScaledTouchSlop();
        final Runnable openProject = () -> {
            try {
                activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(PROJECT_URL)));
            } catch (RuntimeException ignored) {
                // No browser.
            }
        };
        handle.setOnTouchListener(new View.OnTouchListener() {
            float downX, downY, startX, startY;
            boolean moved, longPressed;
            final Runnable longPress = () -> {
                longPressed = true;
                openProject.run();
            };

            @Override
            public boolean onTouch(View v, MotionEvent ev) {
                switch (ev.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX = ev.getRawX();
                        downY = ev.getRawY();
                        startX = root.getTranslationX();
                        startY = root.getTranslationY();
                        moved = false;
                        longPressed = false;
                        handler.postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = ev.getRawX() - downX, dy = ev.getRawY() - downY;
                        if (!moved && (Math.abs(dx) > slop || Math.abs(dy) > slop)) {
                            moved = true;
                            handler.removeCallbacks(longPress);
                        }
                        if (moved) {
                            root.setTranslationX(startX + dx);
                            root.setTranslationY(startY + dy);
                        }
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        handler.removeCallbacks(longPress);
                        return true;
                    case MotionEvent.ACTION_UP:
                        handler.removeCallbacks(longPress);
                        if (!moved && !longPressed) panel.setVisibility(panel.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
                        return true;
                    default:
                        return false;
                }
            }
        });

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                (int) (230 * dp), FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START);
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
                boolean showBadge = value >= 0 && panel.getVisibility() != View.VISIBLE;
                badge.setVisibility(showBadge ? View.VISIBLE : View.GONE);
                if (showBadge) badge.setText(String.format(Locale.US, "%.0f FPS", value));
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
