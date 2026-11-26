package org.battlerealms.nativeport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

public final class LauncherActivity extends Activity {
    private static final int PICK_GAME_FOLDER = 41;
    private static final int BG = Color.rgb(24, 28, 26);
    private static final int PANEL = Color.rgb(38, 43, 39);
    private static final int GOLD = Color.rgb(218, 178, 86);
    private static final int INK = Color.rgb(234, 231, 217);
    private static final int DIM = Color.rgb(167, 172, 158);
    private TextView folderStatus;
    private TextView runtimeStatus;
    private Button play;
    private Button entrypointTrace;
    private boolean entrypointTraceStarted;
    private Uri gameTree;
    private String storageRootDocumentId;
    private String gameRootDocumentId;
    private String gameFolderDisplayPath;
    private File gameRootPath;
    private SharedPreferences settings;

    static { System.loadLibrary("brnative"); }
    private static native boolean nativeRuntimeAvailable();
    static native void nativeSetDiagnosticsAttemptId(String attemptId);
    private static native String nativeRuntimeStatus();
    static native void nativeConfigureDiagnostics(String path);
    private static native void nativeSetDisplayMetrics(int width, int height);
    private static native void nativeSetUserLocale(String languageTag);
    private static native int nativeLiftedFunctionCount();
    private static native boolean nativeEnableFileBridge(Class<?> safProvider);
    private static native String nativePrepareMainExecutable();
    private static native String nativeExecuteMainEntrypoint();

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        File logFile = new File(getFilesDir(), "battlerealms_native.log");
        LaunchDiagnostics.initialize(logFile);
        try { nativeConfigureDiagnostics(logFile.getAbsolutePath()); }
        catch (RuntimeException | UnsatisfiedLinkError error) {
            LaunchDiagnostics.failure("NATIVE_DIAGNOSTICS_CONFIGURATION_FAILED", error);
        }
        LaunchDiagnostics.event("LAUNCHER_ACTIVITY_CREATED", "saved_state=%s", state != null);
        Point screen = new Point();
        getWindowManager().getDefaultDisplay().getRealSize(screen);
        nativeSetDisplayMetrics(Math.max(screen.x, screen.y), Math.min(screen.x, screen.y));
        nativeSetUserLocale(Locale.getDefault().toLanguageTag());
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        settings = getSharedPreferences("launcher", MODE_PRIVATE);
        String saved = settings.getString("game_tree", "");
        if (!saved.isEmpty()) {
            gameTree = Uri.parse(saved);
            try {
                String treeRoot = DocumentsContract.getTreeDocumentId(gameTree);
                storageRootDocumentId = settings.getString("storage_root_document_id", treeRoot);
                gameRootDocumentId = settings.getString("game_root_document_id", treeRoot);
                gameFolderDisplayPath = settings.getString("game_folder_display_path", "");
                LaunchDiagnostics.event("SAVED_GAME_TREE_PERMISSION",
                        "tree_uri=%s persisted_read=%s tree_root_id=%s game_root_id=%s",
                        gameTree, hasPersistedReadPermission(gameTree), storageRootDocumentId,
                        gameRootDocumentId);
            } catch (IllegalArgumentException error) {
                LaunchDiagnostics.failure("SAVED_GAME_FOLDER_URI_INVALID", error);
                gameTree = null;
            }
        }
        String savedPath = settings.getString("game_path", "");
        if (!savedPath.isEmpty()) gameRootPath = new File(savedPath);
        migrateLegacyTreePath();
        buildUi();
        refreshStatus();
        runIntentRequestedTrace();
    }

    @Override protected void onResume() {
        super.onResume();
        if (folderStatus != null) {
            migrateLegacyTreePath();
            refreshStatus();
            runIntentRequestedTrace();
        }
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView v = new TextView(this);
        v.setText(value);
        v.setTextColor(color);
        v.setTextSize(size);
        v.setTypeface(Typeface.create("sans-serif-condensed", bold ? Typeface.BOLD : Typeface.NORMAL));
        v.setPadding(dp(4), dp(5), dp(4), dp(5));
        return v;
    }

    private int dp(float n) { return (int)(n * getResources().getDisplayMetrics().density + 0.5f); }

    private Button button(String label, boolean primary) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(true);
        b.setTextColor(primary ? BG : INK);
        b.setTextSize(16);
        b.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        b.setBackgroundTintList(android.content.res.ColorStateList.valueOf(primary ? GOLD : PANEL));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(48));
        lp.topMargin = dp(9);
        b.setLayoutParams(lp);
        return b;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setPadding(dp(28), dp(22), dp(28), dp(22));
        scroll.addView(root);

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(left, new LinearLayout.LayoutParams(0, -1, 1f));
        left.addView(text("BATTLE REALMS", 34, GOLD, true));
        left.addView(text("NATIVE ARM64  \u00b7  DEVELOPMENT PREVIEW", 13, DIM, true));
        left.addView(text("Original game files stay in the folder you select. The launcher only reads them.", 14, INK, false));
        folderStatus = text("Choose the Battle Realms Complete folder.", 13, DIM, false);
        left.addView(folderStatus);
        Button choose = button("GAME FOLDER", false);
        choose.setOnClickListener(v -> chooseFolder());
        left.addView(choose);
        play = button("PLAY", true);
        play.setEnabled(false);
        play.setAlpha(0.42f);
        play.setOnClickListener(v -> startGame());
        left.addView(play);
        entrypointTrace = button("RUN ENTRYPOINT TRACE", false);
        entrypointTrace.setEnabled(false);
        entrypointTrace.setAlpha(0.42f);
        entrypointTrace.setOnClickListener(v -> runEntrypointTrace());
        left.addView(entrypointTrace);
        Button exit = button("EXIT", false);
        exit.setOnClickListener(v -> finishAndRemoveTask());
        left.addView(exit);

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(18), dp(14), dp(18), dp(14));
        panel.setBackgroundTintList(android.content.res.ColorStateList.valueOf(PANEL));
        panel.setBackgroundColor(PANEL);
        LinearLayout.LayoutParams panelLp = new LinearLayout.LayoutParams(dp(330), -1);
        panelLp.leftMargin = dp(26);
        root.addView(panel, panelLp);
        panel.addView(text("PORT STATUS", 17, GOLD, true));
        runtimeStatus = text("", 13, INK, false);
        panel.addView(runtimeStatus);
        Button controls = button("TOUCH CONTROLS", false);
        controls.setOnClickListener(v -> showControls());
        panel.addView(controls);
        panel.addView(text("Controls are a design preview. They will activate when the game input bridge is implemented and tested.", 12, DIM, false));
        Button shareDiagnostics = button("SHARE DIAGNOSTICS", false);
        shareDiagnostics.setOnClickListener(v -> shareDiagnostics(shareDiagnostics));
        panel.addView(shareDiagnostics);
        panel.addView(text("Shares the timestamped native runtime log as a file. Review it before sending.", 12, DIM, false));
        setContentView(scroll);
    }

    private void shareDiagnostics(Button source) {
        File logFile = new File(getFilesDir(), "battlerealms_native.log");
        if (!logFile.isFile() || logFile.length() == 0L) {
            new AlertDialog.Builder(this)
                    .setTitle("No diagnostics yet")
                    .setMessage("Launch the game first. The native runtime log will appear here.")
                    .setPositiveButton("OK", null)
                    .show();
            return;
        }
        Uri logUri = Uri.parse("content://" + getPackageName() + ".diagnostics/battlerealms_native.log");
        try (android.os.ParcelFileDescriptor ignored = getContentResolver().openFileDescriptor(logUri, "r")) {
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_SUBJECT, "Battle Realms Android diagnostics");
            send.putExtra(Intent.EXTRA_TEXT, "Battle Realms Native ARM64 runtime log from " +
                    android.os.Build.MODEL + " / Android " + android.os.Build.VERSION.RELEASE);
            send.putExtra(Intent.EXTRA_STREAM, logUri);
            send.setClipData(ClipData.newRawUri("Battle Realms diagnostics log", logUri));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(send, "Share Battle Realms diagnostics log"));
        } catch (Exception error) {
            LaunchDiagnostics.failure("DIAGNOSTICS_SHARE_FAILED", error);
            new AlertDialog.Builder(this)
                    .setTitle("Could not share diagnostics")
                    .setMessage("The runtime log could not be opened: " + error.getClass().getSimpleName())
                    .setPositiveButton("OK", null)
                    .show();
        }
    }

    private void chooseFolder() {
        if (!hasAllFilesAccess()) {
            requestAllFilesAccess();
            return;
        }
        new DirectGameFolderPickerDialog(this, storageRoots(), gameRootPath,
                new DirectGameFolderPickerDialog.Listener() {
                    @Override public void onFolderChosen(File folder) {
                        gameRootPath = folder;
                        gameFolderDisplayPath = folder.getAbsolutePath();
                        settings.edit().putString("game_path", gameFolderDisplayPath)
                                .remove("game_tree").remove("storage_root_document_id")
                                .remove("game_root_document_id").apply();
                        gameTree = null;
                        storageRootDocumentId = null;
                        gameRootDocumentId = null;
                        LaunchDiagnostics.event("GAME_FOLDER_SELECTED_DIRECT",
                                "path=%s listable=%s executable=%s levels=%s scripts=%s",
                                gameFolderDisplayPath, folder.listFiles() != null,
                                new File(folder, "Battle_Realms_F.exe").isFile(),
                                new File(folder, "Levels").isDirectory(),
                                new File(folder, "Scripts").isDirectory());
                        refreshStatus();
                    }
                }).show();
    }

    private boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    private void requestAllFilesAccess() {
        LaunchDiagnostics.event("ALL_FILES_ACCESS_REQUIRED", "sdk=%d granted=%s",
                Build.VERSION.SDK_INT, hasAllFilesAccess());
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (RuntimeException error) {
            LaunchDiagnostics.failure("ALL_FILES_ACCESS_SETTINGS_APP_FAILED", error);
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (RuntimeException fallbackError) {
                LaunchDiagnostics.failure("ALL_FILES_ACCESS_SETTINGS_FAILED", fallbackError);
                new AlertDialog.Builder(this).setTitle("Storage access unavailable")
                        .setMessage("Android could not open All files access settings: " + fallbackError)
                        .setPositiveButton("OK", null).show();
            }
        }
    }

    private void migrateLegacyTreePath() {
        if (gameRootPath != null || !hasAllFilesAccess() || gameTree == null) return;
        try {
            String id = DocumentsContract.getTreeDocumentId(gameTree);
            if (id.startsWith("primary:")) {
                File candidate = new File(Environment.getExternalStorageDirectory(), id.substring("primary:".length()));
                if (candidate.isDirectory()) {
                    gameRootPath = candidate.getCanonicalFile();
                    gameFolderDisplayPath = gameRootPath.getAbsolutePath();
                    settings.edit().putString("game_path", gameFolderDisplayPath).apply();
                    LaunchDiagnostics.event("LEGACY_SAF_PATH_MIGRATED", "path=%s", gameFolderDisplayPath);
                }
            }
        } catch (Exception error) {
            LaunchDiagnostics.failure("LEGACY_SAF_PATH_MIGRATION_FAILED", error);
        }
    }

    private java.util.List<File> storageRoots() {
        java.util.ArrayList<File> candidates = new java.util.ArrayList<>();
        java.util.ArrayList<File> roots = new java.util.ArrayList<>();
        java.util.HashSet<String> seen = new java.util.HashSet<>();
        candidates.add(Environment.getExternalStorageDirectory());
        try {
            StorageManager manager = (StorageManager) getSystemService(STORAGE_SERVICE);
            if (manager != null) for (StorageVolume volume : manager.getStorageVolumes()) {
                File directory = null;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) directory = volume.getDirectory();
                if (directory != null) candidates.add(directory);
            }
        } catch (RuntimeException error) {
            LaunchDiagnostics.failure("STORAGE_VOLUME_ENUMERATION_FAILED", error);
        }
        File[] storageEntries = new File("/storage").listFiles();
        if (storageEntries != null) for (File entry : storageEntries)
            if (!entry.getName().equals("emulated") && !entry.getName().equals("self")) candidates.add(entry);
        for (File candidate : candidates) try {
            File canonical = candidate.getCanonicalFile();
            if (canonical.isDirectory() && canonical.listFiles() != null && seen.add(canonical.getPath())) roots.add(canonical);
        } catch (Exception ignored) {}
        LaunchDiagnostics.event("STORAGE_ROOTS_ENUMERATED", "count=%d paths=%s", roots.size(), roots);
        return roots;
    }

    private boolean hasPersistedReadPermission(Uri tree) {
        if (tree == null) return false;
        try {
            for (android.content.UriPermission permission : getContentResolver().getPersistedUriPermissions())
                if (tree.equals(permission.getUri()) && permission.isReadPermission()) return true;
        } catch (RuntimeException error) {
            LaunchDiagnostics.failure("GAME_FOLDER_PERMISSION_QUERY_FAILED", error);
        }
        return false;
    }

    private void chooseStorageLocation() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(i, PICK_GAME_FOLDER);
        } catch (RuntimeException error) {
            LaunchDiagnostics.failure("GAME_STORAGE_PICKER_START_FAILED", error);
            new AlertDialog.Builder(this).setTitle("Could not open storage picker")
                    .setMessage(error.toString()).setPositiveButton("OK", null).show();
        }
    }

    private void showGameFolderPicker() {
        new GameFolderPickerDialog(this, gameTree, storageRootDocumentId,
                new GameFolderPickerDialog.Listener() {
                    @Override public void onFolderChosen(Uri treeUri, String treeRootId,
                            String folderId, String displayPath) {
                        gameTree = treeUri;
                        storageRootDocumentId = treeRootId;
                        gameRootDocumentId = folderId;
                        gameFolderDisplayPath = displayPath;
                        settings.edit().putString("game_tree", gameTree.toString())
                                .putString("storage_root_document_id", storageRootDocumentId)
                                .putString("game_root_document_id", gameRootDocumentId)
                                .putString("game_folder_display_path", gameFolderDisplayPath)
                                .apply();
                        LaunchDiagnostics.event("GAME_FOLDER_SELECTED", "path=%s root_document_id=%s",
                                gameFolderDisplayPath, gameRootDocumentId);
                        refreshStatus();
                    }

                    @Override public void onChangeLocation() {
                        chooseStorageLocation();
                    }
                }).show();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_GAME_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        gameTree = data.getData();
        int flags = data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION;
        try {
            if ((flags & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0)
                throw new SecurityException("The storage picker did not grant read access.");
            getContentResolver().takePersistableUriPermission(gameTree, flags);
            if (!hasPersistedReadPermission(gameTree))
                throw new SecurityException("Android did not retain the selected folder's read grant.");
            storageRootDocumentId = DocumentsContract.getTreeDocumentId(gameTree);
            gameRootDocumentId = storageRootDocumentId;
            boolean persistedReadPermission = false;
            for (android.content.UriPermission permission : getContentResolver().getPersistedUriPermissions()) {
                if (gameTree.equals(permission.getUri()) && permission.isReadPermission()) {
                    persistedReadPermission = true;
                    break;
                }
            }
            LaunchDiagnostics.event("GAME_STORAGE_TREE_GRANTED",
                    "tree_uri=%s result_flags=0x%08x persisted_read=%s tree_root_id=%s",
                    gameTree, data.getFlags(), persistedReadPermission, storageRootDocumentId);
            settings.edit().putString("game_tree", gameTree.toString())
                    .putString("storage_root_document_id", storageRootDocumentId)
                    .putString("game_root_document_id", gameRootDocumentId)
                    .apply();
            showGameFolderPicker();
        } catch (SecurityException | IllegalArgumentException error) {
            LaunchDiagnostics.failure("GAME_FOLDER_PERMISSION_FAILED", error);
            gameTree = null;
            storageRootDocumentId = null;
            gameRootDocumentId = null;
            new AlertDialog.Builder(this).setTitle("Folder permission was not granted")
                    .setMessage("Choose a folder again and allow read access so Battle Realms can open its game files.")
                    .setPositiveButton("CHOOSE FOLDER", (d, which) -> chooseStorageLocation())
                    .setNegativeButton("CANCEL", null).show();
        }
    }

    private boolean hasChild(String expected) {
        if (gameRootPath != null && hasAllFilesAccess()) {
            File child = findCaseInsensitive(gameRootPath, expected);
            return child != null && child.isDirectory();
        }
        if (gameTree == null) return false;
        Cursor cursor = null;
        try {
            String rootId = gameRootDocumentId != null
                    ? gameRootDocumentId : DocumentsContract.getTreeDocumentId(gameTree);
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(gameTree, rootId);
            cursor = getContentResolver().query(children,
                    new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null);
            while (cursor != null && cursor.moveToNext()) {
                String name = cursor.getString(0);
                if (name != null && expected.equalsIgnoreCase(name)) return true;
            }
        } catch (Exception error) {
            LaunchDiagnostics.failure("GAME_FOLDER_SCAN_FAILED", error);
        } finally { if (cursor != null) cursor.close(); }
        return false;
    }

    private void refreshStatus() {
        boolean storageAccess = hasAllFilesAccess();
        if (storageAccess && gameRootPath == null && gameTree != null) migrateLegacyTreePath();
        boolean exe = isMainExecutableValid();
        boolean levels = hasChild("Levels");
        boolean scripts = hasChild("Scripts");
        if (!storageAccess) folderStatus.setText("All files access is needed to browse your game folder. Tap GAME FOLDER to grant it.");
        else if (gameRootPath == null && gameTree == null) folderStatus.setText("Choose the Battle Realms Complete folder.");
        else folderStatus.setText((exe ? "[OK]" : "[MISSING/INVALID]") + " Battle_Realms_F.exe\n" +
                (levels ? "[OK]" : "[MISSING]") + " Levels   " +
                (scripts ? "[OK]" : "[MISSING]") + " Scripts\n" +
                (gameFolderDisplayPath != null && !gameFolderDisplayPath.isEmpty() ? gameFolderDisplayPath : gameTree));
        boolean fileBridge = false;
        if (gameRootPath != null && storageAccess) {
            try {
                SafGameFiles.configurePath(getApplicationContext(), gameRootPath.getCanonicalPath());
                fileBridge = nativeEnableFileBridge(SafGameFiles.class);
                LaunchDiagnostics.event("FILE_BRIDGE_VALIDATION", "available=%s backend=direct-readonly path=%s",
                        fileBridge, gameRootPath);
            } catch (RuntimeException | UnsatisfiedLinkError | java.io.IOException error) {
                LaunchDiagnostics.failure("FILE_BRIDGE_VALIDATION_FAILED", error);
            }
        } else if (gameTree != null && storageAccess) {
            try {
                SafGameFiles.configure(getApplicationContext(), gameTree,
                        gameRootDocumentId != null ? gameRootDocumentId : storageRootDocumentId);
                fileBridge = nativeEnableFileBridge(SafGameFiles.class);
                LaunchDiagnostics.event("FILE_BRIDGE_VALIDATION", "available=%s", fileBridge);
            } catch (RuntimeException | UnsatisfiedLinkError error) {
                LaunchDiagnostics.failure("FILE_BRIDGE_VALIDATION_FAILED", error);
            }
        }
        String pePreparation = "";
        if (exe && fileBridge) {
            try {
                pePreparation = nativePrepareMainExecutable();
                LaunchDiagnostics.event("PE_PREPARATION_RESULT", "%s", pePreparation);
            }
            catch (RuntimeException | UnsatisfiedLinkError error) {
                LaunchDiagnostics.failure("PE_PREPARATION_JNI_FAILED", error);
                pePreparation = error.toString();
            }
        }
        boolean runtime = false;
        try { runtime = nativeRuntimeAvailable(); }
        catch (UnsatisfiedLinkError error) { LaunchDiagnostics.failure("NATIVE_RUNTIME_QUERY_FAILED", error); }
        int liftedFunctions = 0;
        try { liftedFunctions = nativeLiftedFunctionCount(); } catch (UnsatisfiedLinkError ignored) {}
        runtimeStatus.setText(runtime
                ? "Original PE and ARM64 lifted code are prepared. Game launch is experimental; rendering, input, audio and gameplay need device validation."
                : (liftedFunctions + " lifted functions linked.\n" +
                    (!pePreparation.isEmpty() ? pePreparation + "\n" : "") +
                    (fileBridge
                        ? "Game source files are opened read-only; game writes use the private app overlay."
                        : "Select the game folder to configure the native file bridge.")));
        boolean ready = storageAccess && exe && levels && scripts && runtime;
        play.setEnabled(ready);
        play.setAlpha(ready ? 1.0f : 0.42f);
        String readiness = "exe=" + exe + ", levels=" + levels + ", scripts=" + scripts +
                ", file_bridge=" + fileBridge + ", native=" + runtime;
        LaunchDiagnostics.event("PLAY_READINESS_UPDATED", "%s reason=%s", readiness,
                runtime ? "ready" : safeRuntimeStatus());
        boolean entrypointPrepared = !pePreparation.isEmpty() && pePreparation.startsWith("Main PE mapped");
        boolean canTrace = entrypointPrepared && !entrypointTraceStarted;
        entrypointTrace.setEnabled(canTrace);
        entrypointTrace.setAlpha(canTrace ? 1.0f : 0.42f);
    }

    private void runIntentRequestedTrace() {
        if (!getIntent().getBooleanExtra("br_trace_entrypoint", false) ||
                entrypointTrace == null || !entrypointTrace.isEnabled()) return;
        getIntent().removeExtra("br_trace_entrypoint");
        entrypointTrace.post(this::runEntrypointTrace);
    }

    private void runEntrypointTrace() {
        if (entrypointTraceStarted || !entrypointTrace.isEnabled()) return;
        entrypointTraceStarted = true;
        entrypointTrace.setEnabled(false);
        entrypointTrace.setAlpha(0.42f);
        // The diagnostic executes the original entrypoint, which can remain in
        // the game's main loop. Do not let Play start a second caller against
        // the same process-wide x86 CPU context while that run is active.
        play.setEnabled(false);
        play.setAlpha(0.42f);
        runtimeStatus.setText("Tracing original startup. An unimplemented Windows API will stop the diagnostic run and be recorded in logcat.");
        new Thread(() -> {
            String result;
            try { result = nativeExecuteMainEntrypoint(); }
            catch (RuntimeException | UnsatisfiedLinkError error) {
                LaunchDiagnostics.failure("ENTRYPOINT_TRACE_START_FAILED", error);
                result = "Entrypoint trace could not start: " + error.getClass().getSimpleName();
            }
            final String status = result;
            runOnUiThread(() -> runtimeStatus.setText(status));
        }, "BattleRealmsEntryTrace").start();
    }

    private boolean isMainExecutableValid() {
        if (gameRootPath != null && hasAllFilesAccess()) {
            File executable = findCaseInsensitive(gameRootPath, "Battle_Realms_F.exe");
            if (executable == null || !executable.isFile()) return false;
            try (java.io.InputStream in = new java.io.FileInputStream(executable)) {
                byte[] dos = new byte[64];
                if (!readFully(in, dos) || dos[0] != 'M' || dos[1] != 'Z') return false;
                int peOffset = (dos[0x3c] & 255) | ((dos[0x3d] & 255) << 8) |
                        ((dos[0x3e] & 255) << 16) | ((dos[0x3f] & 255) << 24);
                if (peOffset < 64 || peOffset > 1024 * 1024 || !skipFully(in, peOffset - 64)) return false;
                byte[] header = new byte[26];
                if (!readFully(in, header)) return false;
                int machine = (header[4] & 255) | ((header[5] & 255) << 8);
                int magic = (header[24] & 255) | ((header[25] & 255) << 8);
                return header[0] == 'P' && header[1] == 'E' && header[2] == 0 && header[3] == 0 && machine == 0x14c && magic == 0x10b;
            } catch (Exception error) {
                LaunchDiagnostics.failure("GAME_EXECUTABLE_VALIDATION_FAILED", error);
                return false;
            }
        }
        if (gameTree == null) return false;
        Cursor cursor = null;
        try {
            String rootId = gameRootDocumentId != null
                    ? gameRootDocumentId : DocumentsContract.getTreeDocumentId(gameTree);
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(gameTree, rootId);
            cursor = getContentResolver().query(children,
                    new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null);
            String documentId = null;
            while (cursor != null && cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name != null && "Battle_Realms_F.exe".equalsIgnoreCase(name)) {
                    documentId = cursor.getString(0);
                    break;
                }
            }
            if (documentId == null) return false;
            Uri file = DocumentsContract.buildDocumentUriUsingTree(gameTree, documentId);
            try (java.io.InputStream in = getContentResolver().openInputStream(file)) {
                if (in == null) return false;
                byte[] dos = new byte[64];
                if (!readFully(in, dos) || dos[0] != 'M' || dos[1] != 'Z') return false;
                int peOffset = (dos[0x3c] & 255) | ((dos[0x3d] & 255) << 8) |
                        ((dos[0x3e] & 255) << 16) | ((dos[0x3f] & 255) << 24);
                if (peOffset < 64 || peOffset > 1024 * 1024 || !skipFully(in, peOffset - 64)) return false;
                byte[] header = new byte[26];
                if (!readFully(in, header)) return false;
                int machine = (header[4] & 255) | ((header[5] & 255) << 8);
                int magic = (header[24] & 255) | ((header[25] & 255) << 8);
                return header[0] == 'P' && header[1] == 'E' && header[2] == 0 && header[3] == 0 && machine == 0x14c && magic == 0x10b;
            }
        } catch (Exception error) {
            LaunchDiagnostics.failure("GAME_EXECUTABLE_VALIDATION_FAILED", error);
            return false;
        } finally { if (cursor != null) cursor.close(); }
    }

    private File findCaseInsensitive(File parent, String name) {
        File direct = new File(parent, name);
        if (direct.exists()) return direct;
        File[] children = parent.listFiles();
        if (children == null) return null;
        for (File child : children) if (child.getName().equalsIgnoreCase(name)) return child;
        return null;
    }

    private boolean readFully(java.io.InputStream in, byte[] data) throws java.io.IOException {
        int offset = 0;
        while (offset < data.length) {
            int count = in.read(data, offset, data.length - offset);
            if (count < 0) return false;
            offset += count;
        }
        return true;
    }

    private boolean skipFully(java.io.InputStream in, int amount) throws java.io.IOException {
        while (amount > 0) {
            long skipped = in.skip(amount);
            if (skipped <= 0) {
                if (in.read() < 0) return false;
                skipped = 1;
            }
            amount -= (int) skipped;
        }
        return true;
    }

    private void showControls() {
        LinearLayout options = new LinearLayout(this);
        options.setPadding(dp(20), dp(8), dp(20), dp(4));
        options.setOrientation(LinearLayout.VERTICAL);
        addSetting(options, "DRAG THE MAP", "Pan the camera by dragging", "pan_invert", false);
        addSetting(options, "RIGHT CLICK ON TAP", "Use a tap for the alternate mouse command", "tap_right", false);
        addSetting(options, "SHOW KEYBOARD BUTTON", "Allow opening Android text input in game", "keyboard_button", true);
        addSetting(options, "LARGE HUD", "Use a larger touch target layout", "large_hud", false);
        TextView note = text("Settings save on this device. Game input is not connected yet, so these options have no in-game effect in this preview.", 12, DIM, false);
        options.addView(note);
        new android.app.AlertDialog.Builder(this)
                .setTitle("Touch controls")
                .setView(options)
                .setPositiveButton("DONE", null).show();
    }

    private void addSetting(LinearLayout parent, String label, String summary, String key, boolean defaultValue) {
        Switch toggle = new Switch(this);
        toggle.setText(label + "\n" + summary);
        toggle.setTextColor(INK);
        toggle.setTextSize(13);
        toggle.setPadding(0, dp(7), 0, dp(7));
        toggle.setChecked(settings.getBoolean(key, defaultValue));
        toggle.setOnCheckedChangeListener((button, checked) -> settings.edit().putBoolean(key, checked).apply());
        parent.addView(toggle, new LinearLayout.LayoutParams(-1, -2));
    }

    private void startGame() {
        final String attempt = java.util.UUID.randomUUID().toString();
        LaunchDiagnostics.setAttemptId(attempt);
        LaunchDiagnostics.event("PLAY_CLICK_RECEIVED", "source=launcher_button");
        LaunchDiagnostics.event("PLAY_VALIDATION_STARTED", "");
        try {
            nativeSetDiagnosticsAttemptId(attempt);
            boolean available = nativeRuntimeAvailable();
            String reason = available ? "ready" : safeRuntimeStatus();
            LaunchDiagnostics.event("PLAY_VALIDATION_RESULT", "available=%s reason=%s", available, reason);
            if (!available) {
                new AlertDialog.Builder(this).setTitle("Game runtime is not ready")
                        .setMessage(reason).setPositiveButton("OK", null).show();
                return;
            }
            Intent intent = new Intent(this, GameActivity.class).putExtra("launch_attempt_id", attempt);
            LaunchDiagnostics.event("ACTIVITY_START_REQUESTED", "target=GameActivity");
            startActivity(intent);
        } catch (RuntimeException | UnsatisfiedLinkError error) {
            LaunchDiagnostics.failure("PLAY_LAUNCH_FAILED", error);
            new AlertDialog.Builder(this).setTitle("Could not start Battle Realms")
                    .setMessage(error.toString()).setPositiveButton("OK", null).show();
        }
    }

    private String safeRuntimeStatus() {
        try { return nativeRuntimeStatus(); }
        catch (RuntimeException | UnsatisfiedLinkError error) { return "Runtime status query failed: " + error; }
    }
}
