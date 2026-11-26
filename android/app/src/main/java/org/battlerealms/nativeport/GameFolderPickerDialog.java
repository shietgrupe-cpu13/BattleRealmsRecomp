package org.battlerealms.nativeport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;

/** SAF-only in-app folder navigator. It never requests broad storage access. */
final class GameFolderPickerDialog {
    interface Listener {
        void onFolderChosen(Uri treeUri, String treeRootId, String gameFolderId,
                            String displayPath);
        void onChangeLocation();
    }

    private static final String EXECUTABLE = "Battle_Realms_F.exe";
    private final Activity activity;
    private final Uri treeUri;
    private final String treeRootId;
    private final Listener listener;
    private final ArrayList<Node> path = new ArrayList<>();
    private final ArrayList<Node> folders = new ArrayList<>();
    private final ArrayList<String> rows = new ArrayList<>();
    private TextView location;
    private TextView contents;
    private ListView list;
    private AlertDialog dialog;
    private boolean folderReady;
    private String selectedId;

    GameFolderPickerDialog(Activity activity, Uri treeUri, String treeRootId,
                           Listener listener) {
        this.activity = activity;
        this.treeUri = treeUri;
        this.treeRootId = treeRootId;
        this.listener = listener;
    }

    void show() {
        LaunchDiagnostics.event("GAME_FOLDER_PICKER_OPEN", "tree_uri=%s tree_root_id=%s",
                treeUri, treeRootId);
        Node root = readNode(treeRootId);
        if (root == null) {
            showFailure("Android could not read the selected storage folder. Choose it again.");
            return;
        }
        path.clear();
        path.add(root);

        LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(18);
        panel.setPadding(pad, dp(6), pad, 0);

        TextView instructions = new TextView(activity);
        instructions.setText("Browse to the folder containing Battle_Realms_F.exe. Select the game folder, not the EXE file.");
        instructions.setTextSize(14);
        instructions.setPadding(0, dp(6), 0, dp(10));
        panel.addView(instructions);

        location = new TextView(activity);
        location.setTextSize(13);
        location.setPadding(0, dp(4), 0, dp(4));
        panel.addView(location);

        contents = new TextView(activity);
        contents.setTextSize(13);
        contents.setPadding(0, dp(4), 0, dp(8));
        panel.addView(contents);

        list = new ListView(activity);
        panel.addView(list, new LinearLayout.LayoutParams(-1, dp(210)));
        list.setOnItemClickListener((parent, view, position, id) -> {
            int directoryIndex = position - (path.size() > 1 ? 1 : 0);
            if (path.size() > 1 && position == 0) {
                path.remove(path.size() - 1);
                refresh();
            } else if (directoryIndex >= 0 && directoryIndex < folders.size()) {
                path.add(folders.get(directoryIndex));
                refresh();
            }
        });

        dialog = new AlertDialog.Builder(activity)
                .setTitle("Select Battle Realms folder")
                .setView(panel)
                .setNegativeButton("CANCEL", null)
                .setNeutralButton("CHANGE LOCATION", (d, which) -> listener.onChangeLocation())
                .setPositiveButton("USE THIS FOLDER", null)
                .create();
        dialog.setOnShowListener(ignored -> {
            dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener(v -> {
                if (!folderReady || selectedId == null) return;
                StringBuilder savedPath = new StringBuilder();
                for (Node node : path) {
                    if (savedPath.length() != 0) savedPath.append('/');
                    savedPath.append(node.name);
                }
                listener.onFolderChosen(treeUri, treeRootId, selectedId, savedPath.toString());
                dialog.dismiss();
            });
            int availableWidth = activity.getResources().getDisplayMetrics().widthPixels - dp(32);
            Window window = dialog.getWindow();
            if (window != null) window.setLayout(Math.min(availableWidth, dp(620)),
                    WindowManager.LayoutParams.WRAP_CONTENT);
            refresh();
        });
        dialog.show();
    }

    private void refresh() {
        Node current = path.get(path.size() - 1);
        location.setText(pathText());
        folders.clear();
        rows.clear();
        folderReady = false;
        selectedId = null;
        boolean hasExe = false;
        boolean hasLevels = false;
        boolean hasScripts = false;
        boolean cursorNull = false;
        int childCount = 0;
        StringBuilder sampleNames = new StringBuilder();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, current.id);
        try {
            String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE};
            try (Cursor cursor = activity.getContentResolver().query(children, columns,
                    null, null, null)) {
                cursorNull = cursor == null;
                if (cursor != null) {
                    while (cursor.moveToNext()) {
                        ++childCount;
                        String id = cursor.getString(0);
                        String name = cursor.getString(1);
                        String mime = cursor.getString(2);
                        if (name == null) continue;
                        if (sampleNames.length() < 240) {
                            if (sampleNames.length() != 0) sampleNames.append('|');
                            sampleNames.append(name);
                        }
                        boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
                        if (name.equalsIgnoreCase(EXECUTABLE) && !directory) hasExe = true;
                        if (name.equalsIgnoreCase("Levels") && directory) hasLevels = true;
                        if (name.equalsIgnoreCase("Scripts") && directory) hasScripts = true;
                        if (directory) folders.add(new Node(id, name));
                    }
                }
            }
        } catch (Exception error) {
            LaunchDiagnostics.failure("GAME_FOLDER_BROWSE_FAILED", error);
            contents.setText("Could not read this folder. Check its access permission or choose another location.");
        }
        LaunchDiagnostics.event("GAME_FOLDER_LIST_RESULT",
                "tree_uri=%s tree_root_id=%s current_document_id=%s children_uri=%s cursor_null=%s children=%d folders=%d has_exe=%s has_levels=%s has_scripts=%s sample_names=%s",
                treeUri, treeRootId, current.id, children, cursorNull, childCount, folders.size(),
                hasExe, hasLevels, hasScripts, sampleNames);
        Collections.sort(folders, new Comparator<Node>() {
            @Override public int compare(Node a, Node b) {
                return a.name.compareToIgnoreCase(b.name);
            }
        });
        if (path.size() > 1) rows.add("\u2191  ..");
        for (Node folder : folders) rows.add("\u25a0  " + folder.name);
        list.setAdapter(new ArrayAdapter<>(activity,
                android.R.layout.simple_list_item_1, rows));

        if (hasExe && hasLevels && hasScripts) {
            contents.setText("Found Battle_Realms_F.exe, Levels, and Scripts. This looks like the game folder.");
            folderReady = true;
            selectedId = current.id;
        } else if (hasExe) {
            contents.setText("Found Battle_Realms_F.exe, but Levels or Scripts is missing here.");
        } else if (folders.isEmpty()) {
            contents.setText("No folders to browse. Choose the folder that contains Battle_Realms_F.exe.");
        } else {
            contents.setText("This is a storage folder. Open a folder below until the game files are found.");
        }
        if (dialog != null && dialog.isShowing())
            dialog.getButton(DialogInterface.BUTTON_POSITIVE).setEnabled(folderReady);
    }

    private String pathText() {
        StringBuilder value = new StringBuilder();
        for (Node node : path) {
            if (value.length() != 0) value.append(" / ");
            value.append(node.name);
        }
        return value.toString();
    }

    private Node readNode(String id) {
        try {
            Uri document = DocumentsContract.buildDocumentUriUsingTree(treeUri, id);
            String[] columns = {DocumentsContract.Document.COLUMN_DISPLAY_NAME};
            try (Cursor cursor = activity.getContentResolver().query(document, columns,
                    null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    String name = cursor.getString(0);
                    if (name != null) return new Node(id, name);
                }
            }
        } catch (Exception error) {
            LaunchDiagnostics.failure("GAME_FOLDER_ROOT_QUERY_FAILED", error);
        }
        return null;
    }

    private void showFailure(String message) {
        new AlertDialog.Builder(activity).setTitle("Folder access unavailable")
                .setMessage(message).setPositiveButton("OK", null).show();
    }

    private int dp(int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + 0.5f);
    }

    private static final class Node {
        final String id;
        final String name;
        Node(String id, String name) { this.id = id; this.name = name; }
    }
}
