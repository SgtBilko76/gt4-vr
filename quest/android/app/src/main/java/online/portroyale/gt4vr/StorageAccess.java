package online.portroyale.gt4vr;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

/**
 * Asks for the shared-storage access the game needs to read the user's ISO from /sdcard/&lt;data dir&gt;.
 * Before 0.3.2 only the 2D start screen (LauncherActivity) asked, but the release apps start the VR
 * activity directly: on a fresh install nobody asked and the game showed a black screen. Android 11+
 * (Quest, Pico 4 Ultra): "all files access" settings page; Android 10 (Pico 4): the runtime permission.
 * Asked once per process start; the native side (quest_main.cpp wait_for_user_disc) shows what is missing
 * in the headset and starts the game as soon as access is there.
 */
final class StorageAccess {
    private static final String TAG = "GT4VR-Storage";
    private static boolean asked = false;

    private StorageAccess() {}

    static boolean granted(Activity a) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return a.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    static void request(Activity a) {
        if (asked || granted(a)) return;
        asked = true;
        try {
            if (Build.VERSION.SDK_INT >= 30) {
                try {
                    a.startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                               Uri.parse("package:" + a.getPackageName())));
                } catch (Exception e) {
                    a.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            } else {
                a.requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                                                   Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
            }
            Log.i(TAG, "storage access requested");
        } catch (Exception e) {
            Log.w(TAG, "cannot ask for storage access: " + e);
        }
    }
}
