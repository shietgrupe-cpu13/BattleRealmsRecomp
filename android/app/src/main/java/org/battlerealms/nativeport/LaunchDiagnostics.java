package org.battlerealms.nativeport;

import android.os.Process;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** Append-only Java side of the shared launcher/native session log. */
final class LaunchDiagnostics {
    private static final Object LOCK = new Object();
    private static final String TAG = "BattleRealmsLaunch";
    private static File file;
    private static String attemptId = "";

    private LaunchDiagnostics() {}

    static void initialize(File path) {
        synchronized (LOCK) { file = path; }
        event("JAVA_DIAGNOSTICS_READY", "path=%s", path.getAbsolutePath());
    }

    static void setAttemptId(String id) {
        synchronized (LOCK) { attemptId = id == null ? "" : id; }
    }

    static void event(String name, String format, Object... args) {
        writeEvent("I", name, format, args);
    }

    private static void writeEvent(String level, String name, String format, Object... args) {
        String message;
        try { message = String.format(Locale.ROOT, format, args); }
        catch (RuntimeException e) { message = "format_error=" + e + " raw=" + format; }
        long now = System.currentTimeMillis();
        String utc = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.ROOT) {{
            setTimeZone(java.util.TimeZone.getTimeZone("UTC"));
        }}.format(new Date(now));
        String attempt;
        File target;
        synchronized (LOCK) { attempt = attemptId; target = file; }
        String record = "{\"ts_utc\":" + quote(utc) + ",\"mono_ms\":" + android.os.SystemClock.elapsedRealtime() +
                ",\"level\":" + quote(level) + ",\"tag\":" + quote(TAG) + ",\"event\":" + quote(name) +
                ",\"pid\":" + Process.myPid() + ",\"tid\":" + Thread.currentThread().getId() +
                ",\"attempt_id\":" + quote(attempt) + ",\"message\":" + quote(message) + "}\n";
        String log = "event=" + name + " pid=" + Process.myPid() + " tid=" +
                Thread.currentThread().getId() + " attempt=" + (attempt.isEmpty() ? "none" : attempt) + " " + message;
        if ("E".equals(level) || "F".equals(level)) Log.e(TAG, log); else Log.i(TAG, log);
        if (target == null) return;
        byte[] bytes = record.getBytes(StandardCharsets.UTF_8);
        synchronized (LOCK) {
            try (FileOutputStream out = new FileOutputStream(target, true)) { out.write(bytes); out.getFD().sync(); }
            catch (Exception e) { Log.e(TAG, "diagnostics append failed: " + e, e); }
        }
    }

    static void failure(String event, Throwable failure) {
        StringBuilder detail = new StringBuilder(String.valueOf(failure));
        for (StackTraceElement frame : failure.getStackTrace()) detail.append("\n at ").append(frame);
        writeEvent("E", event, "%s", detail);
    }

    private static String quote(String value) {
        StringBuilder out = new StringBuilder(value.length() + 2).append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default: if (c < 0x20) out.append(String.format(Locale.ROOT, "\\u%04x", (int)c)); else out.append(c);
            }
        }
        return out.append('"').toString();
    }
}
