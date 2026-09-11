package el.vision.targetedhide;

import android.util.Log;

import org.lsposed.hiddenapibypass.HiddenApiBypass;

import java.lang.reflect.*;
import java.util.*;

/**
 * Proxies IPackageManager to filter what target apps see through Android's
 * PackageManager APIs (installed-app lists, package lookups, intent resolution).
 *
 * Despite the class name, it filters by both package names and install paths, 
 * mirroring the native side (module.cpp):
 *   - Package rules (e.g. "com.example.app") -> Matched against the package name.
 *   - Path rules    (e.g. "/data/app/...")   -> Matched against the app's install path (sourceDir).
 *
 * Both rule types support "!" exclusions using the same boundary-safe matching
 * logic (see pathMatches()). Exclusions work identically whether you are exempting 
 * a specific app by name or by exact path from a broader hide rule.
 */
public class PackageManagerProxy implements InvocationHandler {
	// Same tag as the native side (module.cpp), so `logcat -s THIDE` shows
    // both native filesystem hooks and this Java-side proxy together.
    private static final String TAG = "THIDE";
	
	// Disabled by default to prevent log spam (broad rules like "/data/app" generate 
    // hundreds of log lines per list refresh). Set to true for temporary debugging 
    // without manually re-adding individual log statements.
    private static final boolean DEBUG_MATCHES = false;
	
    private final Object orig;
    private final List<String> hiddenPackages = new ArrayList<>();
    private final List<String> hiddenPaths = new ArrayList<>();
    private final List<String> excludedPackages = new ArrayList<>();
    private final List<String> excludedPaths = new ArrayList<>();

    private PackageManagerProxy(Object orig, List<String> rules) {
        this.orig = orig;
        
        // Rules arrive from module.cpp as a single flat list, retaining the native 
        // "!" (exclude) and "/" (path) prefix conventions. They are then parsed 
        // and sorted into the four buckets below.
        for (String rule : rules) {
            boolean isExclude = rule.startsWith("!");
            String actualRule = isExclude ? rule.substring(1) : rule;

            if (actualRule.startsWith("/")) {
                if (isExclude) excludedPaths.add(actualRule);
                else hiddenPaths.add(actualRule);
            } else {
                if (isExclude) excludedPackages.add(actualRule);
                else hiddenPackages.add(actualRule);
            }
        }

        for (String p : hiddenPackages) Log.d(TAG, "Loaded HIDE package: '" + p + "'");
        for (String p : hiddenPaths) Log.d(TAG, "Loaded HIDE path: '" + p + "'");
        for (String p : excludedPackages) Log.d(TAG, "Loaded EXCLUDE package: '" + p + "'");
        for (String p : excludedPaths) Log.d(TAG, "Loaded EXCLUDE path: '" + p + "'");
    }

    public static void inject(String[] rules) throws Exception {
        HiddenApiBypass.addHiddenApiExemptions("L");
        List<String> ruleList = Arrays.asList(rules);

        Class<?> activityThread = Class.forName("android.app.ActivityThread");
        Object currentThread = activityThread.getMethod("currentActivityThread").invoke(null);

        Field sPmField = activityThread.getDeclaredField("sPackageManager");
        sPmField.setAccessible(true);
        Object realPm = sPmField.get(null);

        if (realPm == null) {
            realPm = activityThread.getMethod("getPackageManager").invoke(currentThread);
        }

        Class<?> iPmInterface = Class.forName("android.content.pm.IPackageManager");

        Object proxy = Proxy.newProxyInstance(
            iPmInterface.getClassLoader(),
            new Class<?>[]{iPmInterface},
            new PackageManagerProxy(realPm, ruleList)
        );

        sPmField.set(null, proxy);
    }

    @Override
    public Object invoke(Object proxyObj, Method method, Object[] args) throws Throwable {
        String name = method.getName();

        // --- Fast Path: Pre-invoke block for explicit package names ---
        if (isSinglePackageQuery(name) && args != null && args.length > 0 && args[0] instanceof String) {
            String targetPkg = (String) args[0];
            // Exclusions ALWAYS win
            if (!excludedPackages.contains(targetPkg) && hiddenPackages.contains(targetPkg)) {
				if (DEBUG_MATCHES) Log.d(TAG, "HIDE MATCH (fast path): " + name + " -> " + targetPkg);
                return null;
            }
        }

        // --- Call the real PackageManager ---
        Object result = method.invoke(orig, args);
        if (result == null) return null;

        // --- Post-invoke block for path-based single queries ---
        if (isSinglePackageQuery(name)) {
            if (isItemHidden(result)) return null;
        }

        // --- Post-invoke block for lists (Bulk & Intents) ---
        if (isBulkQuery(name) || isIntentQuery(name)) {
            filterListResult(result);
        }

        return result;
    }

    private static boolean isSinglePackageQuery(String name) {
        return name.equals("getPackageInfo") || name.equals("getApplicationInfo")
            || name.equals("getPackageInfoAsUser") || name.equals("getApplicationInfoAsUser")
            || name.equals("getPackageInfo4App") || name.equals("getApplicationInfo4App");
    }

    private static boolean isBulkQuery(String name) {
        return name.equals("getInstalledPackages") || name.equals("getInstalledApplications")
            || name.equals("getInstalledPackagesAsUser") || name.equals("getInstalledApplicationsAsUser")
            || name.equals("getInstalledPackages4App") || name.equals("getInstalledApplications4App");
    }

    private static boolean isIntentQuery(String name) {
        return name.startsWith("queryIntentActivities") || name.startsWith("queryIntentServices")
            || name.startsWith("queryIntentContentProviders") || name.startsWith("resolveActivity")
            || name.startsWith("resolveIntent");
    }

    @SuppressWarnings("unchecked")
    private void filterListResult(Object result) {
        try {
            List<Object> list;
            Method getList = tryGetMethod(result.getClass(), "getList");
            if (getList != null) {
                list = (List<Object>) getList.invoke(result);
            } else if (result instanceof List) {
                list = (List<Object>) result;
            } else {
                return;
            }
            if (list == null) return;

            list.removeIf(this::isItemHidden);
        } catch (Exception ignored) {
        }
    }

    // --- The Master Filter with Exclusions ---
    private boolean isItemHidden(Object item) {
        if (item == null) return false;
        try {
            String pkgName = null;
            Object appInfo = null;
            String className = item.getClass().getName();

            // Extract data
            if (className.equals("android.content.pm.PackageInfo")) {
                pkgName = (String) item.getClass().getField("packageName").get(item);
                appInfo = item.getClass().getField("applicationInfo").get(item);
            } else if (className.equals("android.content.pm.ApplicationInfo")) {
                pkgName = (String) item.getClass().getField("packageName").get(item);
                appInfo = item;
            } else if (className.equals("android.content.pm.ResolveInfo")) {
                String[] infoFields = {"activityInfo", "serviceInfo", "providerInfo"};
                for (String field : infoFields) {
                    try {
                        Object componentInfo = item.getClass().getField(field).get(item);
                        if (componentInfo != null) {
                            pkgName = (String) componentInfo.getClass().getField("packageName").get(componentInfo);
                            appInfo = componentInfo.getClass().getField("applicationInfo").get(componentInfo);
                            break;
                        }
                    } catch (Exception ignored) {}
                }
            }

            // 1. Check EXCLUSIONS first (Whitelist)
            if (pkgName != null && excludedPackages.contains(pkgName)) {
				if (DEBUG_MATCHES) Log.d(TAG, "EXCLUDE MATCH (package): " + pkgName);
                return false; // Safe, do not hide
            }
            if (appInfo != null && !excludedPaths.isEmpty()) {
                String sourceDir = (String) appInfo.getClass().getField("sourceDir").get(appInfo);
                if (sourceDir != null) {
                    for (String path : excludedPaths) {
                        if (pathMatches(sourceDir, path)) {
                            if (DEBUG_MATCHES) Log.d(TAG, "EXCLUDE MATCH (path): '" + sourceDir + "' by rule '" + path + "'");
                            return false; // Safe, do not hide
                        }
                    }
                }
            }

            // 2. Check HIDDEN items (Blacklist)
            if (pkgName != null && hiddenPackages.contains(pkgName)) {
				if (DEBUG_MATCHES) Log.d(TAG, "HIDE MATCH (package): " + pkgName);
                return true;
            }
            if (appInfo != null && !hiddenPaths.isEmpty()) {
                String sourceDir = (String) appInfo.getClass().getField("sourceDir").get(appInfo);
                if (sourceDir != null) {
                    for (String path : hiddenPaths) {
                        if (pathMatches(sourceDir, path)) {
                            if (DEBUG_MATCHES) Log.d(TAG, "HIDE MATCH (path): '" + sourceDir + "' by rule '" + path + "'");
                            return true;
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    // Matches paths using strict boundary checking (prevents "/data/app" matching "/data/app-other")
    private static boolean pathMatches(String sourceDir, String pattern) {
        if (sourceDir.equals(pattern)) return true;
        return sourceDir.length() > pattern.length()
            && sourceDir.startsWith(pattern)
            && sourceDir.charAt(pattern.length()) == '/';
    }

    private static Method tryGetMethod(Class<?> cls, String name) {
        try {
            return cls.getMethod(name);
        } catch (Exception e) {
            return null;
        }
    }
}
