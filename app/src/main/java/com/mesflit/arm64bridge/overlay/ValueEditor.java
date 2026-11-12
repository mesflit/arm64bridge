// SPDX-License-Identifier: GPL-2.0-or-later
package com.mesflit.arm64bridge.overlay;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * In-game value editor: finds a number in the game's memory and changes or freezes it.
 *
 * It runs inside the game's own process and reads and writes that process's own memory (see [Memory]),
 * so it needs neither root nor a debugger, and works for games that run on the bridge (their memory is
 * part of this process) and for native 64-bit games. Usual workflow:
 * search the value shown in the game, change it in the game, refine with the new value, repeat until
 * a few results are left, then edit or freeze them. Only framework classes are used.
 */
final class ValueEditor {
    enum Type {
        DWORD("Dword (32-bit integer)", 4, 4), FLOAT("Float", 4, 4), DOUBLE("Double", 8, 4),
        QWORD("Qword (64-bit integer)", 8, 4), WORD("Word (16-bit integer)", 2, 2), BYTE("Byte", 1, 1);
        final String label;
        final int size, align;
        Type(String label, int size, int align) { this.label = label; this.size = size; this.align = align; }
    }

    private static final int LIST_LIMIT = 100_000;
    private static final int SHOWN = 60;
    private static final int CHUNK = 1 << 20;

    private static ValueEditor instance;

    private Handler main;  // created with the dialog
    private Type type = Type.DWORD;
    private long[] results = new long[0];
    private int count;          // results (entries of [results] when there is no bitmap)
    private long found;         // results of the last search or refine
    private boolean busy;
    private final Map<Long, Object[]> frozen = new LinkedHashMap<>();  // address -> {Type, Number}
    private Thread freezer;

    static synchronized ValueEditor get() {
        if (instance == null) instance = new ValueEditor();
        return instance;
    }

    // ------------------------------------------------------------------ memory

    /** Writable private memory of this process that can hold game values. */
    private static List<long[]> regions() throws IOException {
        List<long[]> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/self/maps"))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.split("\\s+", 6);
                if (f.length < 5 || !f[1].startsWith("rw") || f[1].charAt(3) != 'p') continue;
                String name = f.length > 5 ? f[5].trim() : "";
                if (name.startsWith("/dev/") || name.startsWith("[stack") || name.startsWith("[vvar")
                        || name.startsWith("[vdso") || name.contains("dalvik") || name.contains("kgsl")
                        || name.contains("gralloc") || name.endsWith(".art") || name.endsWith(".oat")
                        || name.endsWith(".vdex") || name.contains("[anon:stack")) continue;
                int dash = f[0].indexOf('-');
                long start = Long.parseUnsignedLong(f[0].substring(0, dash), 16);
                long end = Long.parseUnsignedLong(f[0].substring(dash + 1), 16);
                if (end > start) out.add(new long[]{start, end});
            }
        }
        return out;
    }

    /** How the memory of this process is read and written. */
    interface Memory {
        /** Copies [len] bytes at [addr] into [dst]; false if that memory cannot be read. */
        boolean read(long addr, byte[] dst, int len);

        boolean write(long addr, byte[] src, int len);
    }

    /**
     * Through the bridge runtime, which uses process_vm_readv/process_vm_writev on this process: bad
     * addresses give an error instead of a crash. Falls back to [RawMemory] if the system refuses.
     */
    static final class BridgeMemory implements Memory {
        private Memory fallback;

        private boolean refused(int result) {
            if (result != -1 && result != -38) return false;  // EPERM, ENOSYS
            try {
                fallback = new RawMemory();
            } catch (Throwable ignored) {
                // stays on the bridge calls
            }
            return fallback != null;
        }

        @Override
        public boolean read(long addr, byte[] dst, int len) {
            if (fallback != null) return fallback.read(addr, dst, len);
            int r = GameOverlay.nativeMemRead(addr, dst, len);
            return refused(r) ? fallback.read(addr, dst, len) : r == len;
        }

        @Override
        public boolean write(long addr, byte[] src, int len) {
            if (fallback != null) return fallback.write(addr, src, len);
            int r = GameOverlay.nativeMemWrite(addr, src, len);
            return refused(r) ? fallback.write(addr, src, len) : r == len;
        }
    }

    /**
     * Without the bridge (native 64-bit apps): the Java runtime's raw memory copy. A raw copy crashes on
     * unmapped memory, so every access first asks the kernel whether the pages are mapped.
     * (/proc/self/mem would be simpler, but Android only lets debuggable apps open it.)
     */
    static final class RawMemory implements Memory {
        private final Object unsafe;
        private final Method copy;
        private final ByteBuffer scratch = ByteBuffer.allocateDirect(CHUNK);
        private final long scratchAddr;
        private final long page;

        RawMemory() throws Exception {
            Class<?> u = Class.forName("sun.misc.Unsafe");
            Field f = u.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            unsafe = f.get(null);
            copy = u.getMethod("copyMemory", long.class, long.class, long.class);
            Field address = java.nio.Buffer.class.getDeclaredField("address");
            address.setAccessible(true);
            scratchAddr = address.getLong(scratch);
            long p = 4096;
            try {
                p = android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
            } catch (Throwable ignored) {
                // default
            }
            page = p > 0 ? p : 4096;
        }

        private boolean mapped(long addr, int len) {
            long start = addr & -page, end = (addr + len + page - 1) & -page;
            try {
                android.system.Os.mincore(start, end - start, new byte[(int) ((end - start) / page)]);
                return true;
            } catch (android.system.ErrnoException e) {
                return e.errno != android.system.OsConstants.ENOMEM;
            } catch (Throwable t) {
                return true;  // no such call here: rely on the list of mapped regions
            }
        }

        @Override
        public synchronized boolean read(long addr, byte[] dst, int len) {
            if (len > CHUNK || !mapped(addr, len)) return false;
            try {
                copy.invoke(unsafe, addr, scratchAddr, (long) len);
                scratch.position(0);
                scratch.get(dst, 0, len);
                return true;
            } catch (Throwable t) {
                return false;
            }
        }

        @Override
        public synchronized boolean write(long addr, byte[] src, int len) {
            if (len > CHUNK || !mapped(addr, len)) return false;
            try {
                scratch.position(0);
                scratch.put(src, 0, len);
                copy.invoke(unsafe, scratchAddr, addr, (long) len);
                return true;
            } catch (Throwable t) {
                return false;
            }
        }
    }

    private Memory memory;

    /** [bridged]: the app runs on the bridge runtime, whose memory calls are preferred. */
    synchronized void init(boolean bridged) {
        if (memory != null) return;
        try {
            memory = bridged ? new BridgeMemory() : new RawMemory();
        } catch (Throwable t) {
            memory = null;
        }
    }

    private Memory memory() throws IOException {
        Memory m = memory;
        if (m == null) throw new IOException("this device does not allow reading the app's memory");
        return m;
    }

    private static Number decode(ByteBuffer b, int at, Type t) {
        switch (t) {
            case DWORD: return b.getInt(at);
            case FLOAT: return b.getFloat(at);
            case DOUBLE: return b.getDouble(at);
            case QWORD: return b.getLong(at);
            case WORD: return b.getShort(at);
            default: return b.get(at);
        }
    }

    private static byte[] encode(Type t, double value) {
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        switch (t) {
            case DWORD: b.putInt((int) (long) value); break;
            case FLOAT: b.putFloat((float) value); break;
            case DOUBLE: b.putDouble(value); break;
            case QWORD: b.putLong((long) value); break;
            case WORD: b.putShort((short) (long) value); break;
            default: b.put((byte) (long) value); break;
        }
        return b.array();
    }

    private static boolean equal(Number found, double want, Type t) {
        if (found == null) return false;
        if (t == Type.FLOAT || t == Type.DOUBLE) {
            double v = found.doubleValue();
            return Math.abs(v - want) <= Math.max(0.001, Math.abs(want) * 1e-6);
        }
        return found.longValue() == (long) want;
    }

    /** The value at [addr], or null if that memory is gone. */
    private Number readValue(Memory m, long addr, Type t) {
        byte[] b = new byte[8];
        if (!m.read(addr, b, t.size)) return null;
        return decode(ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN), 0, t);
    }

    /**
     * Results of a search with many matches (a value such as 0 is everywhere): one bit per aligned
     * location of each memory region, so nothing has to be dropped. Once few are left they become a list.
     */
    private static final class Hits {
        final long start, end;
        final int align;
        final java.util.BitSet bits;

        Hits(long start, long length, int align) {
            this.start = start;
            this.end = start + length;
            this.align = align;
            this.bits = new java.util.BitSet((int) (length / align));
        }
    }

    private List<Hits> bitmap;  // non-null while there are too many results for a list

    /** First search: every aligned location of the whole writable memory that holds [want]. */
    private String search(double want) throws IOException {
        Memory m = memory();
        List<Hits> hits = new ArrayList<>();
        long total = 0, scanned = 0;
        byte[] chunk = new byte[CHUNK];
        ByteBuffer buf = ByteBuffer.wrap(chunk).order(ByteOrder.LITTLE_ENDIAN);
        for (long[] reg : regions()) {
            Hits h = null;
            for (long pos = reg[0]; pos < reg[1]; pos += CHUNK) {
                int len = (int) Math.min(CHUNK, reg[1] - pos);
                if (!m.read(pos, chunk, len)) continue;  // pages that cannot be read
                scanned += len;
                for (int i = 0; i + type.size <= len; i += type.align) {
                    if (!equal(decode(buf, i, type), want, type)) continue;
                    if (h == null) h = new Hits(reg[0], reg[1] - reg[0], type.align);
                    h.bits.set((int) ((pos + i - reg[0]) / type.align));
                    total++;
                }
            }
            if (h != null) hits.add(h);
        }
        if (scanned == 0) throw new IOException("this device does not allow reading the app's memory");
        bitmap = hits;
        found = total;
        compact();
        return status();
    }

    /** Keeps the results that now hold [want]. */
    private String refine(double want) throws IOException {
        Memory m = memory();
        if (bitmap != null) {
            long total = 0;
            byte[] chunk = new byte[CHUNK];
            ByteBuffer buf = ByteBuffer.wrap(chunk).order(ByteOrder.LITTLE_ENDIAN);
            for (Hits h : bitmap) {
                int bit = h.bits.nextSetBit(0);
                while (bit >= 0) {
                    // Read the chunk that starts at this result and check every result inside it.
                    long pos = h.start + (long) bit * h.align;
                    int len = (int) Math.min(CHUNK, h.end - pos);
                    boolean readable = m.read(pos, chunk, len);
                    int b = bit;
                    while (b >= 0) {
                        long at = (long) (b - bit) * h.align;
                        if (at + type.size > len) break;  // first result of the next chunk
                        if (readable && equal(decode(buf, (int) at, type), want, type)) total++;
                        else h.bits.clear(b);
                        b = h.bits.nextSetBit(b + 1);
                    }
                    bit = b;
                }
            }
            found = total;
            compact();
        } else {
            int n = 0;
            for (int i = 0; i < count; i++) {
                if (equal(readValue(m, results[i], type), want, type)) results[n++] = results[i];
            }
            count = n;
            found = n;
        }
        return status();
    }

    /** Turns the bitmap into a list once few enough results are left. */
    private void compact() {
        if (found > LIST_LIMIT) {
            results = new long[0];
            count = (int) Math.min(found, Integer.MAX_VALUE);
            return;
        }
        long[] out = new long[(int) found];
        int n = 0;
        for (Hits h : bitmap)
            for (int b = h.bits.nextSetBit(0); b >= 0 && n < out.length; b = h.bits.nextSetBit(b + 1)) out[n++] = h.start + (long) b * h.align;
        results = out;
        count = n;
        bitmap = null;
    }

    private String status() {
        String s = String.format(Locale.US, "%,d results", found);
        if (found > 5000) s += ". Change the value in the game, then refine with the new value.";
        return s;
    }

    private void set(long addr, Type t, double value) throws IOException {
        if (!memory().write(addr, encode(t, value), t.size)) throw new IOException("cannot write there");
    }

    private synchronized void freeze(long addr, Type t, double value, boolean on) {
        if (on) frozen.put(addr, new Object[]{t, value});
        else frozen.remove(addr);
        if (!frozen.isEmpty() && freezer == null) {
            freezer = new Thread(() -> {
                try {
                    Memory m = memory();
                    while (true) {
                        Map<Long, Object[]> copy;
                        synchronized (ValueEditor.this) {
                            if (frozen.isEmpty()) {
                                freezer = null;
                                return;
                            }
                            copy = new LinkedHashMap<>(frozen);
                        }
                        for (Map.Entry<Long, Object[]> e : copy.entrySet()) {
                            Type ft = (Type) e.getValue()[0];
                            m.write(e.getKey(), encode(ft, (Double) e.getValue()[1]), ft.size);
                        }
                        Thread.sleep(100);
                    }
                } catch (IOException | InterruptedException ignored) {
                    synchronized (ValueEditor.this) {
                        freezer = null;
                    }
                }
            }, "arm64bridge-freeze");
            freezer.setDaemon(true);
            freezer.start();
        }
    }

    // ------------------------------------------------------------------ dialog

    void show(final Activity activity) {
        if (main == null) main = new Handler(Looper.getMainLooper());
        final float dp = activity.getResources().getDisplayMetrics().density;
        final int pad = (int) (12 * dp);
        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad / 2, pad, 0);

        final EditText value = new EditText(activity);
        value.setHint("Value shown in the game");
        value.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        value.setSingleLine(true);
        root.addView(value);

        final Spinner typeSpinner = new Spinner(activity);
        String[] labels = new String[Type.values().length];
        for (int i = 0; i < labels.length; i++) labels[i] = Type.values()[i].label;
        typeSpinner.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, labels));
        typeSpinner.setSelection(type.ordinal());
        root.addView(typeSpinner);

        LinearLayout buttons = new LinearLayout(activity);
        final Button newSearch = new Button(activity);
        newSearch.setText("New search");
        final Button refine = new Button(activity);
        refine.setText("Refine");
        final Button setAll = new Button(activity);
        setAll.setText("Set all");
        LinearLayout.LayoutParams w = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        buttons.addView(newSearch, w);
        buttons.addView(refine, w);
        buttons.addView(setAll, w);
        root.addView(buttons);

        final TextView status = new TextView(activity);
        status.setText(count == 0 ? "Search a value that the game shows, for example your coins." : status());
        root.addView(status);

        final LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(list);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, (int) (220 * dp)));

        final TextView help = new TextView(activity);
        help.setTextSize(11);
        help.setText("Tap a result to change or freeze it. Values the game keeps on a server cannot be changed here.");
        root.addView(help);

        final AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle("Value editor")
                .setView(root)
                .setNegativeButton("Close", null)
                .create();

        final Runnable refresh = new Runnable() {
            @Override
            public void run() {
                list.removeAllViews();
                if (count == 0 || count > 5000) {
                    if (count > 5000) {
                        TextView t = new TextView(activity);
                        t.setText("Too many results to list. Refine first.");
                        list.addView(t);
                    }
                    return;
                }
                try {
                    Memory m = memory();
                    for (int i = 0; i < count && i < SHOWN; i++) {
                        final long addr = results[i];
                        Number v = readValue(m, addr, type);
                        boolean isFrozen;
                        synchronized (ValueEditor.this) {
                            isFrozen = frozen.containsKey(addr);
                        }
                        TextView row = new TextView(activity);
                        row.setPadding(0, (int) (8 * dp), 0, (int) (8 * dp));
                        row.setText(String.format(Locale.US, "%x = %s%s", addr, v == null ? "?" : format(v), isFrozen ? "   [frozen]" : ""));
                        if (isFrozen) row.setTextColor(Color.rgb(0, 120, 215));
                        row.setOnClickListener(x -> editOne(activity, addr, this));
                        list.addView(row);
                    }
                    if (count > SHOWN) {
                        TextView t = new TextView(activity);
                        t.setText("… " + (count - SHOWN) + " more");
                        list.addView(t);
                    }
                } catch (IOException e) {
                    status.setText("Cannot read memory: " + e.getMessage());
                }
            }
        };

        final View.OnClickListener runSearch = v -> {
            final Double want = parse(value.getText().toString());
            if (want == null) {
                value.setError("Enter a number");
                return;
            }
            final boolean fresh = v == newSearch || count == 0;
            Type chosen = Type.values()[typeSpinner.getSelectedItemPosition()];
            if (!fresh && chosen != type) {
                status.setText("The type changed: start a new search.");
                return;
            }
            if (busy) return;
            busy = true;
            type = chosen;
            status.setText(fresh ? "Searching…" : "Refining…");
            new Thread(() -> {
                String text;
                try {
                    text = fresh ? search(want) : refine(want);
                } catch (Throwable t) {
                    text = "Search failed: " + t;
                }
                final String done = text;
                main.post(() -> {
                    busy = false;
                    status.setText(done);
                    refresh.run();
                });
            }, "arm64bridge-search").start();
        };
        newSearch.setOnClickListener(runSearch);
        refine.setOnClickListener(runSearch);
        setAll.setOnClickListener(v -> {
            if (count == 0 || count > 5000) {
                status.setText(count == 0 ? "Nothing to set." : "Too many results: refine first.");
                return;
            }
            askNumber(activity, "Set all " + count + " results to", "", n -> {
                int ok = 0;
                for (int i = 0; i < count; i++) {
                    try {
                        set(results[i], type, n);
                        ok++;
                    } catch (IOException ignored) {
                        // skip
                    }
                }
                status.setText(ok + " values changed");
                refresh.run();
            });
        });

        dialog.show();
        refresh.run();
    }

    private void editOne(Activity activity, long addr, Runnable refresh) {
        final boolean isFrozen;
        synchronized (this) {
            isFrozen = frozen.containsKey(addr);
        }
        final EditText input = new EditText(activity);
        input.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        input.setHint("New value");
        new AlertDialog.Builder(activity)
                .setTitle(String.format(Locale.US, "%x (%s)", addr, type.label))
                .setView(input)
                .setPositiveButton("Set", (d, w) -> {
                    Double n = parse(input.getText().toString());
                    if (n == null) return;
                    try {
                        set(addr, type, n);
                        if (isFrozen) freeze(addr, type, n, true);
                    } catch (IOException ignored) {
                        // shown as "?" after the refresh
                    }
                    refresh.run();
                })
                .setNeutralButton(isFrozen ? "Unfreeze" : "Set & freeze", (d, w) -> {
                    if (isFrozen) {
                        freeze(addr, type, 0, false);
                    } else {
                        Double n = parse(input.getText().toString());
                        if (n == null) return;
                        try {
                            set(addr, type, n);
                        } catch (IOException ignored) {
                            return;
                        }
                        freeze(addr, type, n, true);
                    }
                    refresh.run();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private interface NumberCallback { void accept(double n); }

    private static void askNumber(Activity activity, String title, String hint, NumberCallback cb) {
        final EditText input = new EditText(activity);
        input.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        input.setHint(hint);
        new AlertDialog.Builder(activity)
                .setTitle(title)
                .setView(input)
                .setPositiveButton("Set", (d, w) -> {
                    Double n = parse(input.getText().toString());
                    if (n != null) cb.accept(n);
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static Double parse(String s) {
        try {
            return Double.parseDouble(s.trim().replace(',', '.'));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static String format(Number v) {
        if (v instanceof Float || v instanceof Double) return String.format(Locale.US, "%.4f", v.doubleValue());
        return v.toString();
    }

    private ValueEditor() {}
}
