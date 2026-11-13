package online.portroyale.gt4vr;

import android.app.NativeActivity;
import android.os.Bundle;

/** Gran Turismo 4 (USA) v2.00 with online play: libgt4vr_online.so, data /sdcard/GT4HD. The library comes from this activity's android.app.lib_name meta-data; its own process. */
public class OnlineActivity extends NativeActivity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        StorageAccess.request(this);  // the ISO is read from /sdcard (fresh installs: 0.3.2 fix)
    }
}
