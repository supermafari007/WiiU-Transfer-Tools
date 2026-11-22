package com.eldiuca.wiiutransfer;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Proveedor ligero sin dependencias externas para lanzar el instalador de Android
 * directamente sobre el APK descargado (compatible desde Android 9 API 28 en adelante).
 */
public class ApkFileProvider extends ContentProvider {

    public static final String AUTHORITY = "com.eldiuca.wiiutransfer.apkprovider";

    private File resolveApkFile(Uri uri) {
        String seg = (uri != null) ? uri.getLastPathSegment() : null;
        if (seg == null || seg.contains("..") || seg.contains("/") || seg.contains("\\")) {
            seg = "WiiU_Transfer_Tools_Android.apk";
        }
        File cacheApk = new File(getContext().getCacheDir(), seg);
        if (cacheApk.exists()) {
            return cacheApk;
        }
        File dlDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        return new File(dlDir, seg);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return "application/vnd.android.package-archive";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File f = resolveApkFile(uri);
        if (!f.exists()) {
            throw new FileNotFoundException("APK no encontrado: " + f.getAbsolutePath());
        }
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = resolveApkFile(uri);
        String[] cols = (projection != null) ? projection : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor cursor = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) {
                row[i] = f.getName();
            } else if (OpenableColumns.SIZE.equals(cols[i])) {
                row[i] = f.exists() ? f.length() : 0L;
            }
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) { return null; }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { return 0; }
}
