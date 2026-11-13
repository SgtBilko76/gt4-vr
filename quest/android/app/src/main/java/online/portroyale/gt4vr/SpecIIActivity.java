package online.portroyale.gt4vr;

import android.app.NativeActivity;
import android.os.Bundle;

/** Gran Turismo 4 Spec II 1.10 (offline): libgt4vr_specii.so, data /sdcard/GT4SpecII. The library comes from this activity's android.app.lib_name meta-data; its own process. */
public class SpecIIActivity extends NativeActivity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        StorageAccess.request(this);  // the ISO is read from /sdcard (fresh installs: 0.3.2 fix)
    }
}
