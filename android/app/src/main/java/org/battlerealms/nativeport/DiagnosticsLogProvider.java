package org.battlerealms.nativeport;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/** Read-only sharing endpoint for the app's native runtime log. */
public final class DiagnosticsLogProvider extends ContentProvider {
    private static final String LOG_NAME = "battlerealms_native.log";

    @Override public boolean onCreate() { return true; }

    @Override public String getType(Uri uri) {
        requireLogUri(uri);
        return "text/plain";
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                  String[] selectionArgs, String sortOrder) {
        requireLogUri(uri);
        File file = logFile();
        MatrixCursor result = new MatrixCursor(new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        result.addRow(new Object[]{LOG_NAME, file.isFile() ? file.length() : 0L});
        return result;
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        requireLogUri(uri);
        if (!"r".equals(mode)) throw new FileNotFoundException("Diagnostics log is read-only");
        File file = logFile();
        if (!file.isFile()) throw new FileNotFoundException("Diagnostics log has not been created yet");
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Diagnostics log is read-only");
    }

    @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Diagnostics log is read-only");
    }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Diagnostics log is read-only");
    }

    private File logFile() {
        return new File(getContext().getFilesDir(), LOG_NAME);
    }

    private void requireLogUri(Uri uri) {
        if (!"org.battlerealms.nativeport.diagnostics".equals(uri.getAuthority()) ||
                !LOG_NAME.equals(uri.getLastPathSegment()))
            throw new IllegalArgumentException("Only the Battle Realms runtime log can be shared");
    }
}
