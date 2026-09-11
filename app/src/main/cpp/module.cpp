#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdarg>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdio>
#include <dlfcn.h>
#include <dirent.h> 
#include <cstdint> 
#include <algorithm>
#include <stdlib.h>
#include <limits.h>

#include "zygisk.hpp"
#include "dobby.h"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "THIDE", __VA_ARGS__)

#define DEX_PATH "/data/adb/modules/targetedhide/classes.dex"

#define MODULE_CONFIG_DIR "/data/adb/modules/targetedhide/config"

#define TARGET_LIST_PATH MODULE_CONFIG_DIR "/target.txt"

#define PATH_LIST_PATH MODULE_CONFIG_DIR "/path.txt"

#define MAX_PROC_NAME_LEN 256

// =======================================================================
// Shared hidden-path state & Path Normalization
// =======================================================================
// Parsed config lines:
//   "/exact/path" -> exact/prefix match
//   "substring"   -> substring match (no leading '/')
//   "!..."        -> exclude rule
// Substring mode bypasses Android 11+ randomized /data/app directories.
struct PathRule {
    std::string pattern;
    bool isSubstring;
};

static std::vector<PathRule> g_hiddenPaths;
static std::vector<PathRule> g_excludedPaths;

static std::string sanitizePath(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : path) {
        if (c == '/') {
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (part != "." && !part.empty()) {
                parts.push_back(part);
            }
            part.clear();
        } else {
            part += c;
        }
    }
    if (part == "..") {
        if (!parts.empty()) parts.pop_back();
    } else if (part != "." && !part.empty()) {
        parts.push_back(part);
    }

    std::string out = "";
    for (const auto& p : parts) {
        out += "/" + p;
    }
    return out.empty() ? "/" : out;
}

static std::string normalizePath(const std::string &path) {
    if (path.empty()) return path;

    char resolved[PATH_MAX];
    
    // 1. Resolve symlinks and mount namespaces natively
    if (realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }

    // 2. If the file doesn't exist yet (e.g., during creation), resolve its parent directory
    size_t lastSlash = path.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
        std::string dir = path.substr(0, lastSlash);
        std::string file = path.substr(lastSlash); // keep the leading slash
        
        if (realpath(dir.c_str(), resolved) != nullptr) {
            return std::string(resolved) + file;
        }
    }

    // 3. Fallback: sanitize slashes if native resolution fails entirely
    return sanitizePath(path);
}

static bool matchesRule(const std::string &path, const PathRule &rule) {
    if (rule.isSubstring) {
        return path.find(rule.pattern) != std::string::npos;
    }
    if (path == rule.pattern) return true;
    if (path.size() > rule.pattern.size() &&
        path.compare(0, rule.pattern.size(), rule.pattern) == 0 &&
        path[rule.pattern.size()] == '/') {
        return true;
    }
    return false;
}

// True if `path` contains an explicitly excluded sub-path 
// (e.g., path="/a/b", exclude="/a/b/c.txt").
// Ignores substring rules, as they don't represent strict directory structures.
static bool hasNestedExclude(const std::string &path) {
    bool endsWithSlash = !path.empty() && path.back() == '/';
    for (const auto &r: g_excludedPaths) {
        if (r.isSubstring) continue;
        if (r.pattern.size() > path.size() &&
            r.pattern.compare(0, path.size(), path) == 0 &&
            (endsWithSlash || r.pattern[path.size()] == '/')) {
            return true;
        }
    }
    return false;
}

static bool isHidden(const std::string &rawPath) {
    if (rawPath.empty() || rawPath == "/") return false;

    std::string path = normalizePath(rawPath);

    // Exclusions always take priority over hide rules
    for (const auto &r: g_excludedPaths) {
        if (matchesRule(path, r)) return false;
    }
    for (const auto &r: g_hiddenPaths) {
        if (matchesRule(path, r)) {
            // Keep parent folders accessible so the app can reach nested excluded items
            if (!r.isSubstring && hasNestedExclude(path)) {
                continue;
            }
            return true;
        }
    }
    return false;
}

// =======================================================================
// FD Resolution via Kernel & Reentrancy Guard
// =======================================================================
static std::mutex g_fdMutex;
static std::unordered_map<int, std::string> g_fdToPath;

static thread_local bool t_in_hook = false;

class HookGuard {
public:
    HookGuard() { t_in_hook = true; }
    ~HookGuard() { t_in_hook = false; }
};

static std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

static std::string resolveFdPath(int fd) {
    if (fd == AT_FDCWD) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) return normalizePath(cwd);
        return "";
    }
    char linkPath[64], resolved[4096];
    snprintf(linkPath, sizeof(linkPath), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(linkPath, resolved, sizeof(resolved) - 1);
    if (len <= 0) return "";
    resolved[len] = '\0';
    return normalizePath(resolved);
}

static std::string resolveFullPath(int dirfd, const char *pathname) {
    if (!pathname) return "";
    if (pathname[0] == '/') return normalizePath(pathname);
    std::string base = resolveFdPath(dirfd);
    if (base.empty()) return "";
    return joinPath(base, pathname);
}

// =======================================================================
// Hooks
// =======================================================================

typedef int (*open_t)(const char*, int, ...);
static open_t o_open = nullptr;
static int my_open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (t_in_hook) return (flags & O_CREAT) ? o_open(pathname, flags, mode) : o_open(pathname, flags);
    HookGuard guard;

    std::string fullPath = resolveFullPath(AT_FDCWD, pathname);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }

    int fd = (flags & O_CREAT) ? o_open(pathname, flags, mode) : o_open(pathname, flags);
    if (fd >= 0) {
        std::string realPath = resolveFdPath(fd);
        if (!realPath.empty()) {
            std::lock_guard<std::mutex> lock(g_fdMutex);
            g_fdToPath[fd] = realPath;
        }
    }
    return fd;
}

typedef int (*openat_t)(int, const char *, int, ...);
static openat_t o_openat = nullptr;
static int my_openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (t_in_hook) return (flags & O_CREAT) ? o_openat(dirfd, pathname, flags, mode) : o_openat(dirfd, pathname, flags);
    HookGuard guard;

    std::string fullPath = resolveFullPath(dirfd, pathname);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }

    int fd = (flags & O_CREAT) ? o_openat(dirfd, pathname, flags, mode) : o_openat(dirfd, pathname, flags);
    if (fd >= 0) {
        std::string realPath = resolveFdPath(fd);
        if (!realPath.empty()) {
            std::lock_guard<std::mutex> lock(g_fdMutex);
            g_fdToPath[fd] = realPath;
        }
    }
    return fd;
}

typedef int (*close_t)(int);
static close_t o_close = nullptr;
static int my_close(int fd) {
    if (t_in_hook) return o_close(fd);
    HookGuard guard;
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        g_fdToPath.erase(fd);
    }
    return o_close(fd);
}

typedef DIR* (*opendir_t)(const char*);
static opendir_t o_opendir = nullptr;
static DIR* my_opendir(const char* name) {
    if (t_in_hook) return o_opendir(name);
    HookGuard guard;

    std::string fullPath = resolveFullPath(AT_FDCWD, name);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return nullptr;
    }
    return o_opendir(name);
}

typedef struct dirent* (*readdir_t)(DIR*);
static readdir_t o_readdir = nullptr;
static struct dirent* my_readdir(DIR* dirp) {
    if (!dirp) return nullptr;
    if (t_in_hook) return o_readdir(dirp);
    HookGuard guard;

    struct dirent* entry;
    while ((entry = o_readdir(dirp)) != nullptr) {
        std::string name(entry->d_name);
        if (name == "." || name == "..") return entry;

        int fd = dirfd(dirp);
        std::string dirPath;
        if (fd >= 0) {
            std::lock_guard<std::mutex> lock(g_fdMutex);
            auto it = g_fdToPath.find(fd);
            if (it != g_fdToPath.end()) dirPath = it->second;
            else dirPath = resolveFdPath(fd);
        }
        
        if (!dirPath.empty() && isHidden(joinPath(dirPath, name))) continue; 
        return entry;
    }
    return nullptr;
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

typedef long (*getdents64_t)(unsigned int, void *, unsigned int);
static getdents64_t o_getdents64 = nullptr;
static long my_getdents64(unsigned int fd, void *dirp, unsigned int count) {
    if (t_in_hook) return o_getdents64(fd, dirp, count);
    HookGuard guard;

    long nread = o_getdents64(fd, dirp, count);
    if (nread <= 0) return nread;

    std::string dirPath;
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fdToPath.find((int) fd);
        if (it != g_fdToPath.end()) dirPath = it->second;
        else dirPath = resolveFdPath((int)fd);
    }

    if (dirPath.empty()) return nread;

    char *base = (char *) dirp;
    long outOffset = 0, inOffset = 0;

    while (inOffset < nread) {
        auto *entry = (linux_dirent64 *) (base + inOffset);
        std::string name(entry->d_name);
        bool drop = (name != "." && name != ".." && isHidden(joinPath(dirPath, name)));

        if (!drop) {
            if (outOffset != inOffset) memmove(base + outOffset, entry, entry->d_reclen);
            outOffset += entry->d_reclen;
        }
        inOffset += entry->d_reclen;
    }
    return outOffset;
}

typedef int (*stat_t)(const char *, struct stat *);
static stat_t o_stat = nullptr;
static int my_stat(const char *path, struct stat *buf) {
    if (t_in_hook) return o_stat(path, buf);
    HookGuard guard;

    std::string fullPath = resolveFullPath(AT_FDCWD, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_stat(path, buf);
}

typedef int (*lstat_t)(const char *, struct stat *);
static lstat_t o_lstat = nullptr;
static int my_lstat(const char *path, struct stat *buf) {
    if (t_in_hook) return o_lstat(path, buf);
    HookGuard guard;

    std::string fullPath = resolveFullPath(AT_FDCWD, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_lstat(path, buf);
}

typedef int (*fstatat_t)(int, const char *, struct stat *, int);
static fstatat_t o_fstatat = nullptr;
static int my_fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    if (t_in_hook) return o_fstatat(dirfd, path, buf, flags);
    HookGuard guard;

    std::string fullPath = resolveFullPath(dirfd, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_fstatat(dirfd, path, buf, flags);
}

typedef int (*access_t)(const char *, int);
static access_t o_access = nullptr;
static int my_access(const char *path, int mode) {
    if (t_in_hook) return o_access(path, mode);
    HookGuard guard;

    std::string fullPath = resolveFullPath(AT_FDCWD, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_access(path, mode);
}

typedef int (*faccessat_t)(int, const char *, int, int);
static faccessat_t o_faccessat = nullptr;
static int my_faccessat(int dirfd, const char *path, int mode, int flags) {
    if (t_in_hook) return o_faccessat(dirfd, path, mode, flags);
    HookGuard guard;

    std::string fullPath = resolveFullPath(dirfd, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_faccessat(dirfd, path, mode, flags);
}

typedef int (*chdir_t)(const char *);
static chdir_t o_chdir = nullptr;
static int my_chdir(const char *path) {
    if (t_in_hook) return o_chdir(path);
    HookGuard guard;

    std::string fullPath = resolveFullPath(AT_FDCWD, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_chdir(path);
}

struct statx_buf;
typedef int (*statx_t)(int, const char *, int, unsigned int, struct statx_buf *);
static statx_t o_statx = nullptr;
static int my_statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx_buf *buf) {
    if (t_in_hook) return o_statx(dirfd, path, flags, mask, buf);
    HookGuard guard;

    std::string fullPath = resolveFullPath(dirfd, path);
    if (!fullPath.empty() && isHidden(fullPath)) {
        errno = ENOENT;
        return -1;
    }
    return o_statx(dirfd, path, flags, mask, buf);
}

static void installHooks() {
    struct {
        const char *name;
        void *hookFn;
        void **origPtr;
    } hooks[] = {
        {"open",       (void *) my_open,       (void **) &o_open},
        {"openat",     (void *) my_openat,     (void **) &o_openat},
        {"close",      (void *) my_close,      (void **) &o_close},
        {"opendir",    (void *) my_opendir,    (void **) &o_opendir},
        {"readdir",    (void *) my_readdir,    (void **) &o_readdir},
        {"getdents64", (void *) my_getdents64, (void **) &o_getdents64},
        {"stat",       (void *) my_stat,       (void **) &o_stat},
        {"lstat",      (void *) my_lstat,      (void **) &o_lstat},
        {"fstatat",    (void *) my_fstatat,    (void **) &o_fstatat},
        {"access",     (void *) my_access,     (void **) &o_access},
        {"faccessat",  (void *) my_faccessat,  (void **) &o_faccessat},
        {"chdir",      (void *) my_chdir,      (void **) &o_chdir},
        {"statx",      (void *) my_statx,      (void **) &o_statx},
    };

    void* libc_handle = dlopen("libc.so", RTLD_NOW);

    for (auto &h: hooks) {
        void *addr = nullptr;

        if (libc_handle) {
            addr = dlsym(libc_handle, h.name);
            if (!addr && strcmp(h.name, "getdents64") == 0) {
                addr = dlsym(libc_handle, "__getdents64");
            }
        }

        if (!addr) addr = DobbySymbolResolver("libc.so", h.name);
        if (!addr) addr = DobbySymbolResolver(nullptr, h.name);

        if (addr == nullptr) continue;

        DobbyHook(addr, reinterpret_cast<dobby_dummy_func_t>(h.hookFn),
                  reinterpret_cast<dobby_dummy_func_t *>(h.origPtr));
    }

    if (libc_handle) dlclose(libc_handle);
    LOGD("Hooks installed successfully, %zu paths locked down", g_hiddenPaths.size());
}

// =======================================================================
// Zygisk module - Early Companion IPC
// =======================================================================
class TargetedHide : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        // 1. Safety Exit: Unload if args/name are null
        if (args == nullptr || args->nice_name == nullptr) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        if (rawProcess == nullptr) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        
        procName = rawProcess;
        env->ReleaseStringUTFChars(args->nice_name, rawProcess);

        // 2. Connect to companion
        int companionFd = api->connectCompanion();
        if (companionFd < 0) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // 3. Send process name size & string
        uint32_t nameSize = (uint32_t) procName.size();
        write(companionFd, &nameSize, sizeof(uint32_t));
        write(companionFd, procName.data(), nameSize);

        // 4. Read target result from companion
        int ack = 0;
        read(companionFd, &ack, sizeof(int));

        // 5. If not target, close socket & UNLOAD module from memory!
        if (!ack) {
            close(companionFd);
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // App IS a target!
        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);
        isAppTarget = true;

        // 6. Read and normalize paths from companion
        uint32_t pathCount = 0;
        read(companionFd, &pathCount, sizeof(uint32_t));

        for (uint32_t i = 0; i < pathCount; i++) {
            uint32_t len = 0;
            read(companionFd, &len, sizeof(uint32_t));

            std::string p(len, '\0');
            read(companionFd, &p[0], len);
            p = decodeStringViaJava(p);

            bool isExclude = !p.empty() && p[0] == '!';
            std::string pattern = isExclude ? p.substr(1) : p;
            if (pattern.empty()) continue; // guard against a lone "!" line

            bool isSubstring = pattern[0] != '/';
            PathRule rule;
            rule.isSubstring = isSubstring;
            // Only normalize absolute paths; normalizing package substrings would prepending slashes and corrupt them
            rule.pattern = isSubstring ? pattern : normalizePath(pattern);

            auto &list = isExclude ? g_excludedPaths : g_hiddenPaths;
            bool dup = false;
            for (auto &existing: list) {
                if (existing.pattern == rule.pattern && existing.isSubstring == rule.isSubstring) {
                    dup = true;
                    break;
                }
            }
            if (!dup) list.push_back(std::move(rule));
        }

        // 7. Receive classes.dex bytes (0-length means the proxy feature is disabled)
        uint32_t dexSize = 0;
        read(companionFd, &dexSize, sizeof(uint32_t));
        if (dexSize > 0) {
            dexBytes.resize(dexSize);
            read(companionFd, dexBytes.data(), dexSize);
        }

        close(companionFd);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        // Install native filesystem hooks
        if (isAppTarget && !g_hiddenPaths.empty()) {
            installHooks();
            LOGD("Hid %zu paths securely for %s", g_hiddenPaths.size(), procName.c_str());
        }

        if (isAppTarget && !dexBytes.empty()) {
            injectPackageManagerProxy();
        }
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        // Prevent injecting hooks into Android system_server
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::string procName;
    bool isAppTarget = false;
    std::vector<uint8_t> dexBytes;

    // Unchecked JNI exceptions cause undefined behavior and app crashes.
    // This safely catches, logs, and clears them after risky JNI calls.
    bool checkAndClearException(const char *step) {
        if (env->ExceptionCheck()) {
            LOGD("PackageManager proxy failed at: %s", step);
            env->ExceptionClear();
            return true;
        }
        return false;
    }
	
	// Decodes config bytes via Java (UTF-8 with windows-1253 fallback for legacy editors).
    // Runs app-side where JNIEnv exists (companion process lacks a JVM).
    std::string decodeStringViaJava(const std::string &rawBytes) {
        if (rawBytes.empty()) return "";

        jclass strClass = env->FindClass("java/lang/String");
        jmethodID ctor = strClass ? env->GetMethodID(strClass, "<init>", "([BLjava/lang/String;)V") : nullptr;
        if (!strClass || !ctor || checkAndClearException("String init")) return rawBytes;

        jbyteArray jbytes = env->NewByteArray((jsize) rawBytes.size());
        if (checkAndClearException("NewByteArray") || !jbytes) return rawBytes;
        env->SetByteArrayRegion(jbytes, 0, (jsize) rawBytes.size(), (const jbyte *) rawBytes.data());

        // Helper lambda to eliminate duplicated JNI decode boilerplate
        auto decode = [&](const char *charset) -> std::string {
            jstring cset = env->NewStringUTF(charset);
            auto jstr = (jstring) env->NewObject(strClass, ctor, jbytes, cset);
            env->DeleteLocalRef(cset);

            if (checkAndClearException(charset) || !jstr) return "";

            const char *cstr = env->GetStringUTFChars(jstr, nullptr);
            std::string str = cstr ? cstr : "";
            if (cstr) env->ReleaseStringUTFChars(jstr, cstr);
            env->DeleteLocalRef(jstr);
            return str;
        };

        // 1. Try UTF-8 first
        std::string result = decode("UTF-8");

        // 2. Fallback to windows-1253 if replacement characters (\xEF\xBF\xBD) are present
        if (result.empty() || result.find("\xEF\xBF\xBD") != std::string::npos) {
            LOGD("Non-UTF8 path detected, falling back to windows-1253 decoding");
            std::string fallback = decode("windows-1253");
            if (!fallback.empty()) result = fallback;
        }

        env->DeleteLocalRef(jbytes);
        env->DeleteLocalRef(strClass);
        return result.empty() ? rawBytes : result;
    }

    void injectPackageManagerProxy() {
        // Send ALL rules to Java (hidden AND excluded)
        std::vector<std::string> proxyRules;
        for (auto &r: g_hiddenPaths) {
            proxyRules.push_back(r.pattern);
        }
        for (auto &r: g_excludedPaths) {
            proxyRules.push_back("!" + r.pattern); // Keep the '!' prefix for Java to parse
        }
        
        if (proxyRules.empty()) return;

        jclass dexLoaderClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (checkAndClearException("FindClass InMemoryDexClassLoader") || dexLoaderClass == nullptr) return;

        jmethodID dexLoaderCtor = env->GetMethodID(dexLoaderClass, "<init>",
            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (checkAndClearException("GetMethodID InMemoryDexClassLoader ctor")) return;

        jclass threadClass = env->FindClass("java/lang/Thread");
        jmethodID currentThreadMethod = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
        jobject currentThread = env->CallStaticObjectMethod(threadClass, currentThreadMethod);
        jmethodID getContextClassLoaderMethod = env->GetMethodID(threadClass, "getContextClassLoader",
            "()Ljava/lang/ClassLoader;");
        jobject parentClassLoader = env->CallObjectMethod(currentThread, getContextClassLoaderMethod);
        if (checkAndClearException("resolve context classloader") || parentClassLoader == nullptr) return;

        jobject byteBuffer = env->NewDirectByteBuffer(dexBytes.data(), (jlong) dexBytes.size());
        if (checkAndClearException("NewDirectByteBuffer") || byteBuffer == nullptr) return;

        jobject dexClassLoader = env->NewObject(dexLoaderClass, dexLoaderCtor, byteBuffer, parentClassLoader);
        if (checkAndClearException("construct InMemoryDexClassLoader") || dexClassLoader == nullptr) return;

        jclass classLoaderClass = env->GetObjectClass(dexClassLoader);
        jmethodID loadClassMethod = env->GetMethodID(classLoaderClass, "loadClass",
            "(Ljava/lang/String;)Ljava/lang/Class;");
        jstring className = env->NewStringUTF("el.vision.targetedhide.PackageManagerProxy");
        auto proxyClass = (jclass) env->CallObjectMethod(dexClassLoader, loadClassMethod, className);
        if (checkAndClearException("loadClass PackageManagerProxy") || proxyClass == nullptr) return;

        jmethodID injectMethod = env->GetStaticMethodID(proxyClass, "inject", "([Ljava/lang/String;)V");
        if (checkAndClearException("GetStaticMethodID inject") || injectMethod == nullptr) return;

        jclass stringClass = env->FindClass("java/lang/String");
        auto hiddenArray = env->NewObjectArray((jsize) proxyRules.size(), stringClass, nullptr);
        for (size_t i = 0; i < proxyRules.size(); i++) {
            jstring pkg = env->NewStringUTF(proxyRules[i].c_str());
            env->SetObjectArrayElement(hiddenArray, (jsize) i, pkg);
            env->DeleteLocalRef(pkg);
        }

        env->CallStaticVoidMethod(proxyClass, injectMethod, hiddenArray);
        if (checkAndClearException("inject() invocation")) return;

        LOGD("PackageManager proxy active, %zu rules loaded", proxyRules.size());
    }
};

// =======================================================================
// Companion (root side)
// =======================================================================
static bool fileExists(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

static std::vector<std::string> readLines(const char *path) {
    std::vector<std::string> lines;
    FILE *f = fopen(path, "r");
    if (!f) return lines;

    char buf[1024];
    bool firstLine = true;
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);

        if (firstLine && line.size() >= 3 &&
            (unsigned char) line[0] == 0xEF &&
            (unsigned char) line[1] == 0xBB &&
            (unsigned char) line[2] == 0xBF) {
            line.erase(0, 3);
        }
        firstLine = false;

        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty() && line[0] != '#') lines.push_back(line);
    }
    fclose(f);
    return lines;
}

static void companion(int fd) {
    uint32_t nameSize = 0;
    read(fd, &nameSize, sizeof(uint32_t));
    if (nameSize == 0 || nameSize > MAX_PROC_NAME_LEN) {
        int ack = 0;
        write(fd, &ack, sizeof(int));
        close(fd);
        return;
    }
    // 1. Read process name from Zygisk
    std::string procName(nameSize, '\0');
    read(fd, &procName[0], nameSize);

    // 2. Normalize process name (convert ':' to '.')
    std::replace(procName.begin(), procName.end(), ':', '.');

    // 3. Load target.txt via readLines
    std::vector<std::string> targets = readLines(TARGET_LIST_PATH);

    bool isTarget = false;
    for (auto t : targets) {
        // Normalize each line in target.txt
        std::replace(t.begin(), t.end(), ':', '.');

        if (t == procName) {
            isTarget = true;
            break;
        }
    }

    int ack = isTarget ? 1 : 0;
    write(fd, &ack, sizeof(int));

    if (isTarget) {
        std::string perAppFile = std::string(MODULE_CONFIG_DIR) + "/" + procName + ".txt";
        std::vector<std::string> paths = fileExists(perAppFile)
                                              ? readLines(perAppFile.c_str())
                                              : readLines(PATH_LIST_PATH);

        uint32_t pathCount = (uint32_t) paths.size();
        write(fd, &pathCount, sizeof(uint32_t));
        for (auto &p: paths) {
            uint32_t len = (uint32_t) p.size();
            write(fd, &len, sizeof(uint32_t));
            write(fd, p.data(), len);
        }

        // Read and stream classes.dex over IPC socket to bypass SELinux restrictions on /data/adb.
        // A size of 0 is valid and means no PackageManager proxy is configured.
        std::vector<uint8_t> dexBytes;
        FILE *dexFile = fopen(DEX_PATH, "rb");
        if (dexFile) {
            fseek(dexFile, 0, SEEK_END);
            long dexLen = ftell(dexFile);
            fseek(dexFile, 0, SEEK_SET);
            if (dexLen > 0) {
                dexBytes.resize((size_t) dexLen);
                if (fread(dexBytes.data(), 1, (size_t) dexLen, dexFile) != (size_t) dexLen) {
                    dexBytes.clear();
                }
            }
            fclose(dexFile);
        }

        uint32_t dexSize = (uint32_t) dexBytes.size();
        write(fd, &dexSize, sizeof(uint32_t));
        if (dexSize > 0) {
            write(fd, dexBytes.data(), dexSize);
        }
    }

    close(fd);
}

REGISTER_ZYGISK_MODULE(TargetedHide)

REGISTER_ZYGISK_COMPANION(companion)
