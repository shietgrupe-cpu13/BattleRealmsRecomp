package org.battlerealms.nativeport;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Environment;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** Path-based game-folder navigator. Source game files are opened read-only by the runtime bridge. */
final class DirectGameFolderPickerDialog {
    interface Listener { void onFolderChosen(File folder); }

    private final Activity activity;
    private final List<File> roots;
    private final Listener listener;
    private final File initial;

    DirectGameFolderPickerDialog(Activity activity, List<File> roots, File current, Listener listener) {
        this.activity = activity;
        this.roots = roots;
        this.listener = listener;
        this.initial = current != null && current.isDirectory() ? current : Environment.getExternalStorageDirectory();
    }

    void show() { browse(initial); }

    private void browse(File directory) {
        File[] children = directory.listFiles();
        ArrayList<File> folders = new ArrayList<>();
        if (children != null) for (File child : children)
            if (child.isDirectory() && !child.getName().startsWith(".")) folders.add(child);
        Collections.sort(folders, Comparator.comparing(File::getName, String.CASE_INSENSITIVE_ORDER));

        boolean isRoot = containsPath(roots, directory);
        ArrayList<File> otherRoots = new ArrayList<>();
        if (isRoot || directory.getParentFile() == null)
            for (File root : roots) if (!samePath(root, directory)) otherRoots.add(root);
        boolean canUp = !isRoot && directory.getParentFile() != null;

        ArrayList<String> labels = new ArrayList<>();
        if (canUp) labels.add("↑  ..");
        for (File root : otherRoots) labels.add("⇄  " + rootLabel(root));
        for (File folder : folders) labels.add("▰  " + folder.getName());

        boolean ready = looksLikeGameFolder(directory);
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = dp(18);
        content.setPadding(padding, dp(4), padding, 0);
        TextView path = new TextView(activity);
        path.setText(directory.getAbsolutePath());
        path.setTextSize(12);
        path.setPadding(0, dp(4), 0, dp(8));
        content.addView(path);
        TextView status = new TextView(activity);
        status.setText(ready
                ? "Found Battle_Realms_F.exe, Levels, and Scripts."
                : children == null ? "This folder cannot be listed. Choose another location."
                : "Open folders until Battle_Realms_F.exe, Levels, and Scripts are all present.");
        status.setTextSize(13);
        status.setPadding(0, 0, 0, dp(8));
        content.addView(status);
        android.widget.ListView list = new android.widget.ListView(activity);
        android.widget.ArrayAdapter<String> adapter = new android.widget.ArrayAdapter<>(
                activity, android.R.layout.simple_list_item_1, labels);
        list.setAdapter(adapter);
        content.addView(list, new LinearLayout.LayoutParams(-1, dp(260)));

        AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle(isRoot ? rootLabel(directory) : "Select game folder")
                .setView(content)
                .setNegativeButton("CANCEL", null)
                .setPositiveButton("USE THIS FOLDER", null)
                .create();
        list.setOnItemClickListener((parent, view, position, id) -> {
            int index = position;
            if (canUp) {
                if (index == 0) { dialog.dismiss(); browse(directory.getParentFile()); return; }
                --index;
            }
            if (index < otherRoots.size()) {
                File root = otherRoots.get(index);
                dialog.dismiss();
                browse(root);
                return;
            }
            index -= otherRoots.size();
            if (index >= 0 && index < folders.size()) {
                File child = folders.get(index);
                dialog.dismiss();
                browse(child);
            }
        });
        dialog.setOnShowListener(ignored -> {
            Button use = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            use.setEnabled(ready);
            use.setOnClickListener(v -> {
                if (!looksLikeGameFolder(directory)) return;
                try {
                    listener.onFolderChosen(directory.getCanonicalFile());
                    dialog.dismiss();
                } catch (Exception error) {
                    LaunchDiagnostics.failure("GAME_FOLDER_CANONICAL_PATH_FAILED", error);
                }
            });
        });
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(ready);
        LaunchDiagnostics.event("DIRECT_GAME_FOLDER_LISTED",
                "path=%s listable=%s folders=%d storage_roots=%d game_ready=%s",
                directory, children != null, folders.size(), roots.size(), ready);
    }

    private boolean looksLikeGameFolder(File folder) {
        File exe = findCaseInsensitive(folder, "Battle_Realms_F.exe");
        File levels = findCaseInsensitive(folder, "Levels");
        File scripts = findCaseInsensitive(folder, "Scripts");
        return exe != null && exe.isFile() && levels != null && levels.isDirectory() &&
                scripts != null && scripts.isDirectory();
    }

    private File findCaseInsensitive(File parent, String name) {
        File direct = new File(parent, name);
        if (direct.exists()) return direct;
        File[] children = parent.listFiles();
        if (children != null) for (File child : children)
            if (child.getName().equalsIgnoreCase(name)) return child;
        return null;
    }

    private String rootLabel(File root) {
        if (samePath(root, Environment.getExternalStorageDirectory())) return "Internal storage";
        try {
            android.os.storage.StorageManager manager = (android.os.storage.StorageManager)
                    activity.getSystemService(Activity.STORAGE_SERVICE);
            android.os.storage.StorageVolume volume = manager != null ? manager.getStorageVolume(root) : null;
            if (volume != null) return volume.getDescription(activity);
        } catch (RuntimeException ignored) { }
        return root.getName();
    }

    private boolean containsPath(List<File> files, File target) {
        for (File file : files) if (samePath(file, target)) return true;
        return false;
    }

    private boolean samePath(File a, File b) {
        try { return a.getCanonicalPath().equals(b.getCanonicalPath()); }
        catch (Exception ignored) { return a.getAbsolutePath().equals(b.getAbsolutePath()); }
    }

    private int dp(int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + 0.5f);
    }
}
