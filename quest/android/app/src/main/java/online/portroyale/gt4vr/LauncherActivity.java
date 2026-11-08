package online.portroyale.gt4vr;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.List;

/**
 * "Gran Turismo 4 VR" start screen (2D panel): picks the game version. Each version is its own native
 * library + activity + process (the game's globals are not re-entrant, and only one may run), so the
 * other version's process is ended before one starts. An APK with only one version starts it directly.
 */
public class LauncherActivity extends Activity {
    static final class Version {
        final String title, detail, lib, dir, process;
        final Class<?> activity;
        Version(String title, String detail, String lib, String dir, String process, Class<?> activity) {
            this.title = title; this.detail = detail; this.lib = lib; this.dir = dir; this.process = process;
            this.activity = activity;
        }
    }

    static final Version[] VERSIONS = {
        new Version("Gran Turismo 4 Spec II", "Spec II 1.10 mod, single player", "gt4vr_specii",
                    "/sdcard/GT4SpecII", ":specii", SpecIIActivity.class),
        new Version("Gran Turismo 4 Online", "USA v2.00, online races and leaderboard", "gt4vr_online",
                    "/sdcard/GT4HD", ":online", OnlineActivity.class),
    };

    boolean present(Version v) {
        return new File(getApplicationInfo().nativeLibraryDir, "lib" + v.lib + ".so").exists();
    }

    /** The ISO the game will use: disc.iso, else the first *.iso by name (same rule as quest_main). */
    static File findIso(Version v) {
        File fixed = new File(v.dir, "disc.iso");
        if (fixed.isFile()) return fixed;
        File[] files = new File(v.dir).listFiles();
        if (files == null) return null;
        java.util.Arrays.sort(files);
        for (File f : files)
            if (f.isFile() && f.getName().toLowerCase().endsWith(".iso")) return f;
        return null;
    }

    // ---- disc check (same tests as the game: Spec II = boot program PCSX2 CRC 646B2E29; retail v2.00 =
    // CORE.GT4, whose unpacked program is CRC A9D9C0A2 - here identified by the file's size + CRC32).
    static final int KIND_SPECII = 1, KIND_SPECII_OTHER = 2, KIND_RETAIL = 3, KIND_RETAIL_OTHER = 4, KIND_NOT_PS2 = 5;

    static int le32(byte[] b, int o) {
        return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8 | (b[o + 2] & 0xFF) << 16 | (b[o + 3] & 0xFF) << 24;
    }

    /** Reads one root-directory file of an ISO 9660 image, or null. */
    static byte[] isoRootFile(java.io.RandomAccessFile in, String name) throws java.io.IOException {
        byte[] pvd = new byte[2048];
        in.seek(16L * 2048);
        in.readFully(pvd);
        if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1') return null;
        int rootLsn = le32(pvd, 156 + 2), rootLen = le32(pvd, 156 + 10);
        if (rootLen <= 0 || rootLen > (1 << 20)) return null;
        byte[] dir = new byte[rootLen];
        in.seek((long) rootLsn * 2048);
        in.readFully(dir);
        for (int off = 0; off < dir.length;) {
            int len = dir[off] & 0xFF;
            if (len == 0) { off = (off / 2048 + 1) * 2048; continue; }
            if (off + len > dir.length) break;
            int nlen = dir[off + 32] & 0xFF;
            String entry = new String(dir, off + 33, nlen, java.nio.charset.StandardCharsets.ISO_8859_1);
            int semi = entry.indexOf(';');
            if (semi >= 0) entry = entry.substring(0, semi);
            if (entry.equals(name)) {
                int lsn = le32(dir, off + 2), size = le32(dir, off + 10);
                if (size < 0 || size > (64 << 20)) return null;
                byte[] data = new byte[size];
                in.seek((long) lsn * 2048);
                in.readFully(data);
                return data;
            }
            off += len;
        }
        return null;
    }

    static int checkDisc(File iso) {
        try (java.io.RandomAccessFile in = new java.io.RandomAccessFile(iso, "r")) {
            byte[] cnf = isoRootFile(in, "SYSTEM.CNF");
            if (cnf == null) return KIND_NOT_PS2;
            String text = new String(cnf, java.nio.charset.StandardCharsets.ISO_8859_1);
            int p = text.indexOf("cdrom0:\\");
            if (p < 0) return KIND_NOT_PS2;
            String boot = text.substring(p + 8).split("[;\\r\\n]")[0];
            byte[] core = isoRootFile(in, "CORE.GT4");
            if (core != null) {
                java.util.zip.CRC32 crc = new java.util.zip.CRC32();
                crc.update(core);
                return core.length == 2020861 && crc.getValue() == 0x397D5471L ? KIND_RETAIL : KIND_RETAIL_OTHER;
            }
            byte[] elf = isoRootFile(in, boot);
            if (elf == null) return KIND_NOT_PS2;
            int x = 0;
            for (int i = 0; i + 4 <= elf.length; i += 4) x ^= le32(elf, i);
            return x == 0x646B2E29 ? KIND_SPECII : KIND_SPECII_OTHER;
        } catch (Exception e) {
            return KIND_NOT_PS2;
        }
    }

    /** null = the disc fits this version; else the message for the player. */
    static String discProblem(Version v, File iso) {
        if (iso == null) return "No ISO found: copy your disc image to " + v.dir;
        boolean spec = v.process.equals(":specii");
        switch (checkDisc(iso)) {
        case KIND_SPECII:
            return spec ? null : iso.getName() + " is Spec II - it belongs in /sdcard/GT4SpecII";
        case KIND_RETAIL:
            return spec ? iso.getName() + " is the standard GT4 - it belongs in /sdcard/GT4HD" : null;
        case KIND_SPECII_OTHER:
            return spec ? iso.getName() + " is not Spec II 1.10 - patch the GT4 Online beta with Spec II 1.10 first"
                        : iso.getName() + " is not Gran Turismo 4 (USA) v2.00";
        case KIND_RETAIL_OTHER:
            return spec ? iso.getName() + " is the standard GT4, not Spec II 1.10"
                        : iso.getName() + " is another GT4 release - this version needs Gran Turismo 4 (USA) v2.00";
        default:
            return iso.getName() + " is not a PS2 disc image";
        }
    }

    void launch(Version chosen) {
        // End the other version's process if it is still alive in the background.
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> procs = am.getRunningAppProcesses();
        if (procs != null)
            for (Version v : VERSIONS)
                if (v != chosen)
                    for (ActivityManager.RunningAppProcessInfo p : procs)
                        if (p.processName.equals(getPackageName() + v.process)) Process.killProcess(p.pid);
        Intent intent = new Intent(this, chosen.activity);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
        finish();
    }

    TextView text(String s, float size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        // The games read their ISO from /sdcard: all-files access is needed once.
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } catch (Exception ignored) {
            }
        }
        int count = 0;
        Version only = null;
        for (Version v : VERSIONS)
            if (present(v)) { count++; only = v; }
        if (count == 1) {
            launch(only);
            return;
        }
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(12, 16, 28));
        root.setPadding(48, 32, 48, 32);
        // Owner's logo, name and website (Port Royale).
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.logo_royale);
        logo.setAdjustViewBounds(true);
        root.addView(logo, new LinearLayout.LayoutParams(160, 160));
        root.addView(text("Gran Turismo 4 3D VR", 34, Color.WHITE, true));
        TextView by = text("by Sgt.Bilko  ·  portroyale.online", 18, Color.rgb(200, 60, 60), false);
        by.setPaintFlags(by.getPaintFlags() | android.graphics.Paint.UNDERLINE_TEXT_FLAG);
        by.setOnClickListener(view -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://portroyale.online")));
            } catch (Exception ignored) {
            }
        });
        root.addView(by);
        root.addView(text("Choose the game version", 18, Color.rgb(170, 180, 200), false));
        for (final Version v : VERSIONS) {
            if (!present(v)) continue;
            final String problem = discProblem(v, findIso(v));
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setTextSize(24);
            b.setText(v.title + "\n" + (problem == null ? v.detail : problem));
            b.setEnabled(problem == null);
            b.setOnClickListener(view -> launch(v));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                                         LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.topMargin = 28;
            root.addView(b, lp);
        }
        // Recentring here (Meta button) fixes the screen height in the game that follows (owner 2026-11-08).
        TextView tip = text("Tip: hold the Meta button to recenter before choosing - the game screen then sits at eye level.",
                            16, Color.rgb(150, 160, 180), false);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                                     LinearLayout.LayoutParams.WRAP_CONTENT);
        tp.topMargin = 28;
        root.addView(tip, tp);
        setContentView(root);
    }
}
