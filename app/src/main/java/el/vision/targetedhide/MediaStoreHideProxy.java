package el.vision.targetedhide;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.util.Log;

import org.lsposed.hiddenapibypass.HiddenApiBypass;

import java.lang.reflect.*;
import java.util.*;

public class MediaStoreHideProxy implements InvocationHandler {
    private static final String TAG = "THIDE";
    private static final boolean DEBUG_MATCHES = false; // Set to false to avoid logcat spam

    private final Object orig;
    private final List<String> hiddenPaths;
    private final List<String> excludedPaths;
    private final int userId;

    private MediaStoreHideProxy(Object orig, List<String> hiddenPaths, List<String> excludedPaths, int userId) {
        this.orig = orig;
        this.hiddenPaths = hiddenPaths;
        this.excludedPaths = excludedPaths;
        this.userId = userId;
    }

    public static void inject(final String[] rules) {
        new Thread(() -> {
            List<String> hiddenPaths = new ArrayList<>();
            List<String> excludedPaths = new ArrayList<>();
            for (String rule : rules) {
                boolean isExclude = rule.startsWith("!");
                String actual = isExclude ? rule.substring(1) : rule;
                if (!actual.startsWith("/")) continue;
                if (isExclude) excludedPaths.add(actual);
                else hiddenPaths.add(actual);
            }
            if (hiddenPaths.isEmpty()) return;

            try {
                HiddenApiBypass.addHiddenApiExemptions("L");
            } catch (Throwable ignored) {}

            int retries = 0;
            while (retries < 50) {
                try {
                    Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
                    Object activityThread = activityThreadClass.getMethod("currentActivityThread").invoke(null);

                    if (activityThread != null) {
                        Object context = activityThreadClass.getMethod("currentApplication").invoke(null);
                        if (context == null) {
                            try {
                                context = activityThreadClass.getMethod("getSystemContext").invoke(activityThread);
                            } catch (Throwable ignored) {}
                        }

                        if (context != null) {
                            if (doInject(activityThreadClass, activityThread, context, hiddenPaths, excludedPaths)) {
                                return;
                            }
                        }
                    }
                } catch (Throwable t) {
                    if (DEBUG_MATCHES) Log.d(TAG, "MediaStore proxy retry error: " + t);
                }

                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    break;
                }
                retries++;
            }
            Log.d(TAG, "MediaStore proxy: timed out waiting for ActivityThread");
        }).start();
    }

    private static boolean doInject(Class<?> activityThreadClass, Object activityThread, Object context,
                                     List<String> hiddenPaths, List<String> excludedPaths) {
        try {
            int userId;
            try {
                userId = (int) Class.forName("android.os.UserHandle").getMethod("myUserId").invoke(null);
            } catch (Throwable t) {
                userId = 0;
            }

            Method acquireProvider = activityThreadClass.getMethod("acquireProvider",
                Class.forName("android.content.Context"), String.class, int.class, boolean.class);
            Object realProvider = acquireProvider.invoke(activityThread, context, "media", userId, true);
            if (realProvider == null) {
                Log.d(TAG, "MediaStore proxy: couldn't resolve real provider");
                return false;
            }

            Class<?> iContentProviderClass = Class.forName("android.content.IContentProvider");
            Method asBinderMethod = Class.forName("android.os.IInterface").getMethod("asBinder");
            Object realBinder = asBinderMethod.invoke(realProvider);

            Object proxyProvider = Proxy.newProxyInstance(
                iContentProviderClass.getClassLoader(),
                new Class<?>[]{iContentProviderClass},
                new MediaStoreHideProxy(realProvider, hiddenPaths, excludedPaths, userId)
            );

            Field providerMapField = activityThreadClass.getDeclaredField("mProviderMap");
            providerMapField.setAccessible(true);
            Object providerMapObj = providerMapField.get(activityThread);
            if (!(providerMapObj instanceof Map)) {
                Log.d(TAG, "MediaStore proxy: mProviderMap not a Map");
                return false;
            }

            boolean patched = false;
            synchronized (providerMapObj) {
                for (Object record : ((Map<?, ?>) providerMapObj).values()) {
                    Field providerField;
                    try {
                        providerField = record.getClass().getDeclaredField("mProvider");
                    } catch (NoSuchFieldException e) {
                        continue;
                    }
                    providerField.setAccessible(true);
                    Object candidate = providerField.get(record);
                    if (candidate == null) continue;

                    Object candidateBinder = asBinderMethod.invoke(candidate);
                    if (candidateBinder == realBinder) {
                        providerField.set(record, proxyProvider);
                        patched = true;
                        break;
                    }
                }
            }

            if (patched) {
                Log.d(TAG, "MediaStore proxy active, " + hiddenPaths.size() + " paths hidden from queries");
                return true;
            }
        } catch (Throwable t) {
            Log.d(TAG, "MediaStore proxy injection failed: " + t);
        }
        return false;
    }

    @Override
    public Object invoke(Object proxyObj, Method method, Object[] args) throws Throwable {
        Object result = method.invoke(orig, args);

        if (method.getName().equals("query") && result instanceof Cursor) {
            return filterCursor((Cursor) result);
        }

        return result;
    }

    private Cursor filterCursor(Cursor original) {
        try {
            String[] columns = original.getColumnNames();
            MatrixCursor filtered = new MatrixCursor(columns);

            int dataCol = original.getColumnIndex("_data");
            int relPathCol = original.getColumnIndex("relative_path");
            int nameCol = original.getColumnIndex("_display_name");
            int bucketNameCol = original.getColumnIndex("bucket_display_name");
            int volCol = original.getColumnIndex("volume_name");

            while (original.moveToNext()) {
                String path = (dataCol >= 0) ? original.getString(dataCol) : null;

                // Dynamic absolute path reconstruction if _data is null
                if (path == null && relPathCol >= 0 && nameCol >= 0) {
                    String rel = original.getString(relPathCol);
                    String name = original.getString(nameCol);
                    if (rel != null && name != null) {
                        String basePath = "/storage/emulated/" + this.userId + "/";

                        if (volCol >= 0) {
                            String volName = original.getString(volCol);
                            if (volName != null && !volName.equals("external_primary") 
                                    && !volName.equals("external") && !volName.equals("internal")) {
                                basePath = "/storage/" + volName + "/";
                            }
                        }

                        if (rel.startsWith("/")) rel = rel.substring(1);
                        path = basePath + rel + name;
                    }
                }

                boolean shouldHide = false;
                String matchReason = null;

                if (path != null) {
                    if (isHidden(path)) {
                        shouldHide = true;
                        matchReason = "path: " + path;
                    }
                } else if (bucketNameCol >= 0) {
                    String bucketName = original.getString(bucketNameCol);
                    if (bucketName != null && isBucketHidden(bucketName)) {
                        shouldHide = true;
                        matchReason = "bucket_display_name: " + bucketName;
                    }
                }

                if (shouldHide) {
                    if (matchReason != null && matchReason.startsWith("bucket_display_name")) {
                        // Bucket-name matching is inherently ambiguous (no path context at all - just a bare folder name),
                        // so any match here always logs, regardless of DEBUG_MATCHES.
                        Log.d(TAG, "MediaStore HIDE MATCH, ambiguous (" + matchReason + ")");
                    } else if (DEBUG_MATCHES) {
                        Log.d(TAG, "MediaStore HIDE MATCH (" + matchReason + ")");
                    }
                    continue;
                }

                Object[] row = new Object[columns.length];
                for (int i = 0; i < columns.length; i++) row[i] = readColumn(original, i);
                filtered.addRow(row);
            }

            original.close();
            return filtered;
        } catch (Exception e) {
            Log.d(TAG, "MediaStore cursor filtering failed: " + e);
            return original;
        }
    }

    private Object readColumn(Cursor c, int i) {
        switch (c.getType(i)) {
            case Cursor.FIELD_TYPE_INTEGER: return c.getLong(i);
            case Cursor.FIELD_TYPE_FLOAT: return c.getDouble(i);
            case Cursor.FIELD_TYPE_BLOB: return c.getBlob(i);
            case Cursor.FIELD_TYPE_NULL: return null;
            default: return c.getString(i);
        }
    }

    private boolean isHidden(String path) {
        for (String p : excludedPaths) if (pathMatches(path, p)) return false;
        for (String p : hiddenPaths) if (pathMatches(path, p)) return true;
        return false;
    }

    private boolean isBucketHidden(String bucketName) {
        for (String p : hiddenPaths) {
            String path = p;
            while (path.endsWith("/")) {
                path = path.substring(0, path.length() - 1);
            }
            if (path.isEmpty()) continue; // Guards against "/" or root-only rules

            String folderName = path;
            int lastSlash = path.lastIndexOf('/');
            if (lastSlash >= 0) {
                folderName = path.substring(lastSlash + 1);
            }
            if (bucketName.equalsIgnoreCase(folderName)) {
                return true;
            }
        }
        return false;
    }

    private static boolean pathMatches(String path, String pattern) {
        if (path.equals(pattern)) return true;
        return path.length() > pattern.length()
            && path.startsWith(pattern)
            && path.charAt(pattern.length()) == '/';
    }
}
