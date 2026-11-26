package org.battlerealms.nativeport;

import android.app.Application;
import java.io.File;

public final class BattleRealmsApplication extends Application {
    @Override public void onCreate() {
        super.onCreate();
        LaunchDiagnostics.initialize(new File(getFilesDir(), "battlerealms_native.log"));
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            LaunchDiagnostics.failure("JAVA_UNCAUGHT_EXCEPTION", error);
            if (previous != null) previous.uncaughtException(thread, error);
        });
        LaunchDiagnostics.event("APPLICATION_CREATED", "package=%s", getPackageName());
    }
}
