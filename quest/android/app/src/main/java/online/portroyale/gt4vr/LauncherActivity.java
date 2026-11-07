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

    static boolean hasIso(Version v) {
        File[] files = new File(v.dir).listFiles();
        if (files == null) return false;
        for (File f : files)
            if (f.isFile() && f.getName().toLowerCase().endsWith(".iso")) return true;
        return false;
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
        root.addView(text("Gran Turismo 4 VR", 34, Color.WHITE, true));
        root.addView(text("Choose the game version", 18, Color.rgb(170, 180, 200), false));
        for (final Version v : VERSIONS) {
            if (!present(v)) continue;
            final boolean iso = hasIso(v);
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setTextSize(24);
            b.setText(v.title + "\n" + (iso ? v.detail : "No ISO found: copy your disc image to " + v.dir));
            b.setEnabled(iso);
            b.setOnClickListener(view -> launch(v));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                                         LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.topMargin = 28;
            root.addView(b, lp);
        }
        setContentView(root);
    }
}
