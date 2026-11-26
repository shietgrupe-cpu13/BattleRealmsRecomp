package org.battlerealms.nativeport;

import android.app.NativeActivity;
import android.os.Bundle;
import android.view.View;
import android.widget.FrameLayout;

public final class GameActivity extends NativeActivity {
    static { System.loadLibrary("brnative"); }
    private GameTouchHud touchHud;

    static native void nativeGameTouchAction(int action);

    @Override protected void onCreate(Bundle state) {
        String attempt = getIntent() == null ? null : getIntent().getStringExtra("launch_attempt_id");
        LaunchDiagnostics.setAttemptId(attempt);
        try {
            LauncherActivity.nativeConfigureDiagnostics(new java.io.File(getFilesDir(), "battlerealms_native.log").getAbsolutePath());
            LauncherActivity.nativeSetDiagnosticsAttemptId(attempt);
        }
        catch (RuntimeException | UnsatisfiedLinkError error) {
            LaunchDiagnostics.failure("GAME_ACTIVITY_NATIVE_INIT_FAILED", error);
        }
        LaunchDiagnostics.event("GAME_ACTIVITY_CREATE_ENTER", "saved_state=%s", state != null);
        super.onCreate(state);
        View content = findViewById(android.R.id.content);
        if (content instanceof FrameLayout) touchHud = new GameTouchHud(this, (FrameLayout) content);
        LaunchDiagnostics.event("GAME_ACTIVITY_CREATED", "native library load requested by NativeActivity");
    }

    @Override protected void onResume() {
        super.onResume();
        LaunchDiagnostics.event("GAME_ACTIVITY_RESUMED", "");
    }

    @Override protected void onPause() {
        LaunchDiagnostics.event("GAME_ACTIVITY_PAUSED", "");
        super.onPause();
    }

    @Override protected void onDestroy() {
        if (touchHud != null) touchHud.detach();
        LaunchDiagnostics.event("GAME_ACTIVITY_DESTROYED", "changing_configurations=%s", isChangingConfigurations());
        super.onDestroy();
    }
}
