package org.battlerealms.nativeport;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Set;

/** SAF-backed game file access. Original documents are opened with read permission only. */
final class SafGameFiles {
    private static final int GENERIC_READ = 0x80000000;
    private static final int GENERIC_WRITE = 0x40000000;
    private static final int CREATE_NEW = 1;
    private static final int CREATE_ALWAYS = 2;
    private static final int OPEN_EXISTING = 3;
    private static final int OPEN_ALWAYS = 4;
    private static final int TRUNCATE_EXISTING = 5;
    private static final int ERROR_FILE_NOT_FOUND = 2;
    private static final int ERROR_ACCESS_DENIED = 5;
    private static final int ERROR_INVALID_PARAMETER = 87;
    private static final int ERROR_FILE_EXISTS = 80;
    private static final int ERROR_ALREADY_EXISTS = 183;
    private static final int ERROR_PATH_NOT_FOUND = 3;

    private static Context appContext;
    private static Uri gameTree;
    private static String gameRootDocumentId;
    private static File gameRootPath;
    private static final Set<String> deletedPaths = new HashSet<>();
    private static final Map<String, Map<String, DocumentInfo>> sourceChildrenCache = new HashMap<>();
    private static long lookupWindowStartedMs;
    private static long lookupCount;
    private static long providerQueryCount;
    private static long providerQueryNanos;
    private static long providerQueryMaxNanos;
    private static long sourceCacheHits;
    private static long fileOpenCount;
    private static long fileOpenNanos;
    private static long fileOpenMaxNanos;

    private SafGameFiles() {}

    static synchronized void configure(Context context, Uri tree, String rootDocumentId) {
        appContext = context.getApplicationContext();
        gameTree = tree;
        gameRootDocumentId = rootDocumentId;
        gameRootPath = null;
        // The selected source tree is read-only to the game. Rebuild its
        // metadata cache whenever the launcher revalidates the selected tree.
        sourceChildrenCache.clear();
        lookupWindowStartedMs = SystemClock.elapsedRealtime();
        lookupCount = providerQueryCount = providerQueryNanos = providerQueryMaxNanos = 0;
        sourceCacheHits = fileOpenCount = fileOpenNanos = fileOpenMaxNanos = 0;
    }

    /** Configures direct, read-only access to the selected game directory. Writes remain in the private overlay. */
    static synchronized void configurePath(Context context, String rootPath) throws java.io.IOException {
        File canonical = new File(rootPath).getCanonicalFile();
        if (!canonical.isDirectory() || canonical.listFiles() == null)
            throw new IllegalArgumentException("Selected game directory is not readable: " + canonical);
        appContext = context.getApplicationContext();
        gameTree = null;
        gameRootDocumentId = null;
        gameRootPath = canonical;
        sourceChildrenCache.clear();
        deletedPaths.clear();
        lookupWindowStartedMs = SystemClock.elapsedRealtime();
        lookupCount = providerQueryCount = providerQueryNanos = providerQueryMaxNanos = 0;
        sourceCacheHits = fileOpenCount = fileOpenNanos = fileOpenMaxNanos = 0;
        LaunchDiagnostics.event("DIRECT_GAME_FILES_CONFIGURED", "root=%s source_writable=false", canonical);
    }

    /** Returns packed {lastError, fd}; -1 fds are failures. */
    public static synchronized long openFile(String relativePath, int desiredAccess, int disposition) {
        long started = System.nanoTime();
        long result = openFileInternal(relativePath, desiredAccess, disposition);
        long elapsed = System.nanoTime() - started;
        ++fileOpenCount;
        fileOpenNanos += elapsed;
        fileOpenMaxNanos = Math.max(fileOpenMaxNanos, elapsed);
        maybeLogFileLookupStats();
        return result;
    }

    private static long openFileInternal(String relativePath, int desiredAccess, int disposition) {
        if (appContext == null || relativePath == null) return encode(-1, ERROR_FILE_NOT_FOUND);
        boolean readable = (desiredAccess & GENERIC_READ) != 0;
        boolean writable = (desiredAccess & GENERIC_WRITE) != 0;
        if (!readable && !writable) return encode(-1, ERROR_INVALID_PARAMETER);
        try {
            if (deletedPaths.contains(relativePath) && disposition != CREATE_NEW && disposition != CREATE_ALWAYS)
                return encode(-1, ERROR_FILE_NOT_FOUND);
            if (disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
                deletedPaths.remove(relativePath);
            File overlay = overlayPath(relativePath);
            DocumentInfo sourceInfo = findDocument(relativePath);
            boolean overlayExists = overlay.isFile();
            boolean sourceExists = sourceInfo != null && !sourceInfo.directory;
            if (sourceInfo != null && sourceInfo.directory)
                return encode(-1, disposition == CREATE_NEW ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
            boolean existed = overlayExists || sourceExists;

            if (disposition == CREATE_NEW && existed)
                return encode(-1, ERROR_FILE_EXISTS);
            if ((disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING) && !existed)
                return encode(-1, ERROR_FILE_NOT_FOUND);
            if (disposition == TRUNCATE_EXISTING && !writable)
                return encode(-1, ERROR_INVALID_PARAMETER);
            if ((disposition == CREATE_NEW || disposition == CREATE_ALWAYS ||
                    disposition == TRUNCATE_EXISTING) && !writable)
                return encode(-1, ERROR_INVALID_PARAMETER);
            if (disposition < CREATE_NEW || disposition > TRUNCATE_EXISTING)
                return encode(-1, ERROR_INVALID_PARAMETER);

            int lastError = 0;
            if (disposition == CREATE_ALWAYS) {
                if (existed) lastError = ERROR_ALREADY_EXISTS;
            } else if (disposition == OPEN_ALWAYS && existed) {
                lastError = ERROR_ALREADY_EXISTS;
            }

            if (!writable && !overlayExists && sourceExists &&
                    (disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS))
                return openReadOnlySource(sourceInfo, lastError);

            if (writable && !overlayExists && sourceExists && gameRootPath != null &&
                    (disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS)) {
                if (!copyDirectSourceToOverlay(new File(sourceInfo.documentId), overlay))
                    return encode(-1, ERROR_ACCESS_DENIED);
            } else if (writable && !overlayExists && sourceExists &&
                    (disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS)) {
                Uri source = DocumentsContract.buildDocumentUriUsingTree(
                        gameTree, sourceInfo.documentId);
                if (!copySourceToOverlay(source, overlay, sourceInfo))
                    return encode(-1, ERROR_ACCESS_DENIED);
            }

            return openOverlay(overlay, readable, writable, disposition, lastError);
        } catch (ErrnoException error) {
            return encode(-1, mapErrno(error.errno));
        } catch (Exception ignored) {
            return encode(-1, ERROR_ACCESS_DENIED);
        }
    }

    /** Returns Win32 FILE_ATTRIBUTE_* bits, or INVALID_FILE_ATTRIBUTES. */
    public static synchronized int getFileAttributes(String relativePath) {
        if (appContext == null || relativePath == null) return -1;
        try {
            if (deletedPaths.contains(relativePath)) return -1;
            if (".".equals(relativePath)) return gameTree != null || gameRootPath != null ? 0x10 : -1;
            File overlay = overlayPath(relativePath);
            if (overlay.exists()) return overlay.isDirectory() ? 0x10 : 0x20;
            DocumentInfo source = findDocument(relativePath);
            if (source == null) return -1;
            return source.directory ? 0x10 : 0x01;
        } catch (Exception ignored) {
            return -1;
        }
    }

    /** Delete in the private overlay namespace; never mutates the selected source tree. */
    public static synchronized int deleteFile(String relativePath) {
        if (appContext == null || relativePath == null) return ERROR_FILE_NOT_FOUND;
        try {
            File overlay = overlayPath(relativePath);
            boolean exists = overlay.isFile() || findDocument(relativePath) != null;
            if (!exists) return ERROR_FILE_NOT_FOUND;
            if (overlay.exists() && !overlay.delete()) return ERROR_ACCESS_DENIED;
            deletedPaths.add(relativePath);
            return 0;
        } catch (Exception ignored) { return ERROR_ACCESS_DENIED; }
    }

    /** Create a new directory only in the app-private overlay; source SAF files stay read-only. */
    public static synchronized int createDirectory(String templateRelativePath, String newRelativePath) {
        if (appContext == null || templateRelativePath == null || newRelativePath == null)
            return ERROR_INVALID_PARAMETER;
        try {
            int templateAttributes = getFileAttributes(templateRelativePath);
            if (templateAttributes == -1) return ERROR_FILE_NOT_FOUND;
            if ((templateAttributes & 0x10) == 0) return ERROR_ACCESS_DENIED;
            if (".".equals(newRelativePath)) return ERROR_ALREADY_EXISTS;

            File overlay = overlayPath(newRelativePath);
            DocumentInfo sourceTarget = findDocument(newRelativePath);
            if (overlay.exists() || sourceTarget != null) return ERROR_ALREADY_EXISTS;

            String normalized = newRelativePath.replace('\\', '/');
            int slash = normalized.lastIndexOf('/');
            String parent = slash < 0 ? "." : normalized.substring(0, slash);
            int parentAttributes = getFileAttributes(parent);
            if (parentAttributes == -1) return ERROR_PATH_NOT_FOUND;
            if ((parentAttributes & 0x10) == 0) return ERROR_PATH_NOT_FOUND;

            File overlayParent = overlayDirectory(parent);
            if (!overlayParent.isDirectory() && !overlayParent.mkdirs()) return ERROR_ACCESS_DENIED;
            if (!overlay.mkdir()) return overlay.exists() ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED;
            return 0;
        } catch (IllegalArgumentException invalidPath) {
            return ERROR_INVALID_PARAMETER;
        } catch (Exception ignored) {
            return ERROR_ACCESS_DENIED;
        }
    }

    /** Returns tab-separated name, attributes, size, modified-ms records for a Win32 wildcard query. */
    public static synchronized String[] findFiles(String relativeDirectory, String wildcard) {
        if (appContext == null || (gameTree == null && gameRootPath == null) || relativeDirectory == null || wildcard == null)
            return new String[0];
        try {
            Map<String, String> entries = new LinkedHashMap<>();
            File overlayDirectory = overlayDirectory(relativeDirectory);
            File[] overlayChildren = overlayDirectory.listFiles();
            if (overlayChildren != null) for (File child : overlayChildren) {
                if (!wildcardMatch(wildcard, child.getName())) continue;
                int attributes = child.isDirectory() ? 0x10 : 0x20;
                String record = child.getName() + "\t" + attributes + "\t" +
                        (child.isFile() ? child.length() : 0) + "\t" + child.lastModified();
                entries.put(child.getName().toLowerCase(Locale.ROOT), record);
            }

            String documentId = gameRootPath != null ? gameRootPath.getCanonicalPath() :
                    (gameRootDocumentId != null ? gameRootDocumentId : DocumentsContract.getTreeDocumentId(gameTree));
            boolean sourceDirectoryExists = true;
            if (!".".equals(relativeDirectory)) {
                String[] parts = relativeDirectory.replace('\\', '/').split("/");
                for (String part : parts) {
                    if (part.isEmpty() || ".".equals(part) || "..".equals(part)) {
                        sourceDirectoryExists = false;
                        break;
                    }
                    DocumentInfo directory = findChild(documentId, part);
                    if (directory == null || !directory.directory) {
                        sourceDirectoryExists = false;
                        break;
                    }
                    documentId = directory.documentId;
                }
            }
            if (sourceDirectoryExists) {
                for (DocumentInfo child : getSourceChildren(documentId).values()) {
                    if (!wildcardMatch(wildcard, child.name)) continue;
                    String record = child.name + "\t" + (child.directory ? 0x10 : 0x01) + "\t" +
                            Math.max(0, child.size) + "\t" + Math.max(0, child.modified);
                    entries.putIfAbsent(child.name.toLowerCase(Locale.ROOT), record);
                }
            }
            ArrayList<String> result = new ArrayList<>(entries.values());
            Collections.sort(result, Comparator.comparing(s -> s.substring(0, s.indexOf('\t')).toLowerCase(Locale.ROOT)));
            maybeLogFileLookupStats();
            return result.toArray(new String[0]);
        } catch (Exception ignored) {
            return new String[0];
        }
    }

    private static File overlayDirectory(String relativeDirectory) throws Exception {
        File root = new File(appContext.getFilesDir(), "game-data").getCanonicalFile();
        File result = root;
        if (!".".equals(relativeDirectory)) {
            for (String part : relativeDirectory.replace('\\', '/').split("/")) {
                if (part.isEmpty() || ".".equals(part) || "..".equals(part))
                    throw new IllegalArgumentException("invalid game-relative directory");
                result = new File(result, part.toLowerCase(Locale.ROOT));
            }
        }
        result = result.getCanonicalFile();
        if (!result.toPath().startsWith(root.toPath())) throw new IllegalArgumentException("directory escaped overlay");
        return result;
    }

    private static boolean wildcardMatch(String pattern, String name) {
        int p = 0, n = 0, star = -1, retry = 0;
        while (n < name.length()) {
            if (p < pattern.length() && (pattern.charAt(p) == '?' ||
                    Character.toLowerCase(pattern.charAt(p)) == Character.toLowerCase(name.charAt(n)))) {
                p++; n++;
            } else if (p < pattern.length() && pattern.charAt(p) == '*') {
                star = p++; retry = n;
            } else if (star >= 0) {
                p = star + 1; n = ++retry;
            } else return false;
        }
        while (p < pattern.length() && pattern.charAt(p) == '*') p++;
        return p == pattern.length();
    }

    private static long encode(int fd, int error) {
        return ((long) error << 32) | (fd & 0xffffffffL);
    }

    private static int mapErrno(int errno) {
        if (errno == OsConstants.ENOENT) return ERROR_FILE_NOT_FOUND;
        if (errno == OsConstants.EEXIST) return ERROR_FILE_EXISTS;
        if (errno == OsConstants.EACCES || errno == OsConstants.EROFS) return ERROR_ACCESS_DENIED;
        return ERROR_ACCESS_DENIED;
    }

    private static File overlayPath(String relativePath) throws Exception {
        String[] parts = relativePath.replace('\\', '/').split("/");
        File root = new File(appContext.getFilesDir(), "game-data").getCanonicalFile();
        File result = root;
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part))
                throw new IllegalArgumentException("invalid game-relative path");
            result = new File(result, part.toLowerCase(Locale.ROOT));
        }
        result = result.getCanonicalFile();
        if (!result.toPath().startsWith(root.toPath()))
            throw new IllegalArgumentException("game path escaped private overlay");
        return result;
    }

    private static DocumentInfo findDocument(String relativePath) {
        if (gameTree == null && gameRootPath == null) return null;
        ++lookupCount;
        try {
            String documentId = gameRootPath != null ? gameRootPath.getCanonicalPath() :
                    (gameRootDocumentId != null ? gameRootDocumentId : DocumentsContract.getTreeDocumentId(gameTree));
            String[] parts = relativePath.replace('\\', '/').split("/");
            DocumentInfo result = null;
            for (int i = 0; i < parts.length; ++i) {
                String part = parts[i];
                if (part.isEmpty() || ".".equals(part) || "..".equals(part)) return null;
                result = findChild(documentId, part);
                if (result == null) return null;
                documentId = result.documentId;
                if (i + 1 < parts.length && !result.directory) return null;
            }
            return result;
        } catch (Exception ignored) {
            return null;
        }
    }

    private static long openReadOnlySource(DocumentInfo info, int lastError) throws Exception {
        if (gameRootPath != null) {
            File source = new File(info.documentId).getCanonicalFile();
            if (!source.toPath().startsWith(gameRootPath.toPath()) || !source.isFile())
                return encode(-1, ERROR_ACCESS_DENIED);
            ParcelFileDescriptor descriptor = ParcelFileDescriptor.open(
                    source, ParcelFileDescriptor.MODE_READ_ONLY);
            return encode(descriptor.detachFd(), lastError);
        }
        Uri source = DocumentsContract.buildDocumentUriUsingTree(gameTree, info.documentId);
        ParcelFileDescriptor descriptor = appContext.getContentResolver()
                .openFileDescriptor(source, "r");
        if (descriptor != null) {
            try {
                if (descriptor.getStatSize() >= 0) {
                    Os.lseek(descriptor.getFileDescriptor(), 0, OsConstants.SEEK_SET);
                    return encode(descriptor.detachFd(), lastError);
                }
            } catch (ErrnoException ignored) {
                // Providers that return pipes are copied to a seekable private cache below.
            } finally {
                descriptor.close();
            }
        }
        File cached = materializeReadOnlyCopy(source, info);
        ParcelFileDescriptor cachedDescriptor = ParcelFileDescriptor.open(
                cached, ParcelFileDescriptor.MODE_READ_ONLY);
        return encode(cachedDescriptor.detachFd(), lastError);
    }

    private static long openOverlay(File file, boolean readable, boolean writable,
                                    int disposition, int lastError) throws Exception {
        File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            return encode(-1, ERROR_ACCESS_DENIED);
        int flags = OsConstants.O_CLOEXEC;
        if (readable && writable) flags |= OsConstants.O_RDWR;
        else if (writable) flags |= OsConstants.O_WRONLY;
        else flags |= OsConstants.O_RDONLY;
        if (disposition == CREATE_NEW) flags |= OsConstants.O_CREAT | OsConstants.O_EXCL;
        else if (disposition == CREATE_ALWAYS) flags |= OsConstants.O_CREAT | OsConstants.O_TRUNC;
        else if (disposition == OPEN_ALWAYS) flags |= OsConstants.O_CREAT;
        else if (disposition == TRUNCATE_EXISTING) flags |= OsConstants.O_TRUNC;

        FileDescriptor descriptor = Os.open(file.getAbsolutePath(), flags, 0600);
        try {
            ParcelFileDescriptor detached = ParcelFileDescriptor.dup(descriptor);
            return encode(detached.detachFd(), lastError);
        } finally {
            Os.close(descriptor);
        }
    }

    private static boolean copySourceToOverlay(Uri source, File destination, DocumentInfo info) {
        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) return false;
        File partial = new File(destination.getPath() + ".part");
        try (InputStream input = appContext.getContentResolver().openInputStream(source);
             FileOutputStream output = new FileOutputStream(partial, false)) {
            if (input == null) {
                partial.delete();
                return false;
            }
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            output.getFD().sync();
            if (info.size >= 0 && partial.length() != info.size) {
                partial.delete();
                return false;
            }
            if (destination.exists() && !destination.delete()) return false;
            return partial.renameTo(destination);
        } catch (Exception ignored) {
            partial.delete();
            return false;
        }
    }

    private static boolean copyDirectSourceToOverlay(File source, File destination) {
        File partial = new File(destination.getAbsolutePath() + ".partial");
        File parent = destination.getParentFile();
        if (!source.isFile() || (parent != null && !parent.isDirectory() && !parent.mkdirs())) return false;
        try (InputStream input = new java.io.FileInputStream(source);
             FileOutputStream output = new FileOutputStream(partial, false)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            output.getFD().sync();
            if (partial.length() != source.length()) { partial.delete(); return false; }
            if (destination.exists() && !destination.delete()) { partial.delete(); return false; }
            return partial.renameTo(destination);
        } catch (Exception error) {
            LaunchDiagnostics.failure("DIRECT_GAME_FILE_COPY_FAILED", error);
            partial.delete();
            return false;
        }
    }

    private static final class DocumentInfo {
        final String documentId;
        final String name;
        final boolean directory;
        final long size;
        final long modified;

        DocumentInfo(String id, String displayName, boolean isDirectory, long byteSize, long lastModified) {
            documentId = id;
            name = displayName;
            directory = isDirectory;
            size = byteSize;
            modified = lastModified;
        }
    }

    private static DocumentInfo findChild(String parentId, String expectedName) throws Exception {
        return getSourceChildren(parentId).get(expectedName.toLowerCase(Locale.ROOT));
    }

    private static Map<String, DocumentInfo> getSourceChildren(String parentId) throws Exception {
        Map<String, DocumentInfo> cached = sourceChildrenCache.get(parentId);
        if (cached != null) {
            ++sourceCacheHits;
            return cached;
        }
        if (gameRootPath != null) {
            File parent = new File(parentId).getCanonicalFile();
            if (!parent.toPath().startsWith(gameRootPath.toPath()) || !parent.isDirectory()) return Collections.emptyMap();
            File[] children = parent.listFiles();
            Map<String, DocumentInfo> found = new LinkedHashMap<>();
            if (children == null) return Collections.emptyMap();
            for (File child : children) try {
                File canonical = child.getCanonicalFile();
                if (!canonical.toPath().startsWith(gameRootPath.toPath())) continue;
                found.putIfAbsent(child.getName().toLowerCase(Locale.ROOT),
                        new DocumentInfo(canonical.getAbsolutePath(), child.getName(), child.isDirectory(),
                                child.isFile() ? child.length() : 0, child.lastModified()));
            } catch (Exception ignored) {}
            sourceChildrenCache.put(parentId, found);
            return found;
        }
        ContentResolver resolver = appContext.getContentResolver();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(gameTree, parentId);
        String[] columns = {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED
        };
        long queryStarted = System.nanoTime();
        Map<String, DocumentInfo> found = new LinkedHashMap<>();
        boolean complete = false;
        try (Cursor cursor = resolver.query(children, columns, null, null, null)) {
            if (cursor == null) return Collections.emptyMap();
            while (cursor != null && cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name == null) continue;
                String id = cursor.getString(0);
                String mime = cursor.getString(2);
                long size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                long modified = cursor.isNull(4) ? -1 : cursor.getLong(4);
                boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
                found.putIfAbsent(name.toLowerCase(Locale.ROOT),
                        new DocumentInfo(id, name, directory, size, modified));
            }
            complete = true;
        } finally {
            long elapsed = System.nanoTime() - queryStarted;
            ++providerQueryCount;
            providerQueryNanos += elapsed;
            providerQueryMaxNanos = Math.max(providerQueryMaxNanos, elapsed);
        }
        if (complete) sourceChildrenCache.put(parentId, found);
        return found;
    }

    private static void maybeLogFileLookupStats() {
        long now = SystemClock.elapsedRealtime();
        if (lookupWindowStartedMs == 0) lookupWindowStartedMs = now;
        long elapsedMs = now - lookupWindowStartedMs;
        if (elapsedMs < 5000 || (lookupCount == 0 && fileOpenCount == 0 && providerQueryCount == 0)) return;
        LaunchDiagnostics.event("SAF_FILE_IO_WINDOW",
                "window_ms=%d path_lookups=%d source_opens=%d provider_queries=%d provider_query_ms=%.2f " +
                        "provider_query_max_ms=%.2f source_cache_hits=%d source_open_ms_avg=%.2f source_open_max_ms=%.2f",
                elapsedMs, lookupCount, fileOpenCount, providerQueryCount,
                providerQueryNanos / 1000000.0, providerQueryMaxNanos / 1000000.0, sourceCacheHits,
                fileOpenCount == 0 ? 0.0 : fileOpenNanos / (fileOpenCount * 1000000.0),
                fileOpenMaxNanos / 1000000.0);
        lookupWindowStartedMs = now;
        lookupCount = providerQueryCount = providerQueryNanos = providerQueryMaxNanos = 0;
        sourceCacheHits = fileOpenCount = fileOpenNanos = fileOpenMaxNanos = 0;
    }

    private static File materializeReadOnlyCopy(Uri source, DocumentInfo info) throws Exception {
        File cacheDir = new File(appContext.getCacheDir(), "game-files");
        if (!cacheDir.isDirectory() && !cacheDir.mkdirs()) return null;
        String identity = source.toString() + "|" + info.size + "|" + info.modified;
        if (info.size < 0 || info.modified < 0) identity += "|" + System.nanoTime();
        String key = sha256(identity);
        File cached = new File(cacheDir, key + ".bin");
        if (cached.isFile() && (info.size < 0 || cached.length() == info.size)) return cached;
        File partial = new File(cacheDir, key + ".part");
        try (InputStream input = appContext.getContentResolver().openInputStream(source);
             FileOutputStream output = new FileOutputStream(partial, false)) {
            if (input == null) {
                partial.delete();
                return null;
            }
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            output.getFD().sync();
            if (info.size >= 0 && partial.length() != info.size) {
                partial.delete();
                return null;
            }
            if (cached.exists() && !cached.delete()) return null;
            if (!partial.renameTo(cached)) return null;
            return cached;
        } catch (Exception ignored) {
            partial.delete();
            return null;
        }
    }

    private static String sha256(String value) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(value.getBytes("UTF-8"));
        StringBuilder result = new StringBuilder(digest.length * 2);
        for (byte b : digest) result.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return result.toString();
    }
}
