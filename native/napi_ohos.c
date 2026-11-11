// nx2android HarmonyOS NEXT platform layer: NAPI entry of libdmhost.so (P0 skeleton).
//
// Structure follows d2r-mobile-main/dm/harmony/native/napi_ohos.c (MIT, Copyright (c) 2026 jhaizhou): the ArkTS page
// calls start(), the host prepares the sandbox directories and the DM_* environment, and the XComponent surface
// callbacks are the place where the presentation layer will be attached once dm/android/gfx_vk_* is linked in.
//
// P0 scope only: boot log under the hilog tag "nx2a-ohos", DM_ROOT / DM_ROMFS / DM_SAVE / DM_TEMP, an
// executable-memory self-test and the XComponent surface placeholder. The game core is not linked yet.
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <dirent.h>
#include <dlfcn.h>
extern void *dm_ohos_front_window(void);
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "gfx_surface_ohos.h"

#define NX_LOG_DOMAIN 0xD2E1
#define NX_LOG_TAG "nx2a-ohos"

#define PATH_CAP 1024
#define OUT_CAP 8192

static char g_files[PATH_CAP];
static char g_runtime[PATH_CAP + 16];
static char g_romfs[PATH_CAP + 16];
static char g_save[PATH_CAP + 16];
static char g_temp[PATH_CAP + 16];
static char g_surface[64];
static char g_bootinfo[OUT_CAP];
static FILE *g_logf;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started;
static void *g_window;
static uint64_t g_win_w, g_win_h;

// ---- Logging: hilog with the team tag, mirrored into files/dm/boot.log ------------------------------
static void nx_log(LogLevel level, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    OH_LOG_Print(LOG_APP, level, NX_LOG_DOMAIN, NX_LOG_TAG, "%{public}s", msg);
    pthread_mutex_lock(&g_log_lock);
    if (g_logf) {
        fputs(msg, g_logf);
        fputc('\n', g_logf);
        fflush(g_logf);
    }
    pthread_mutex_unlock(&g_log_lock);
}

// Log a boot line and append it to the string shown on the ArkTS page.
static void boot_line(const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    nx_log(LOG_INFO, "boot: %s", msg);
    size_t used = strlen(g_bootinfo);
    if (used + strlen(msg) + 2 < sizeof g_bootinfo) {
        snprintf(g_bootinfo + used, sizeof g_bootinfo - used, "%s\n", msg);
    }
}

static void mkdirs(const char *path) {
    char buf[PATH_CAP + 16];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(buf, 0700);
            *s = '/';
        }
    }
    mkdir(buf, 0700);
}

static int get_str(napi_env env, napi_value v, char *buf, size_t cap) {
    size_t n = 0;
    if (napi_get_value_string_utf8(env, v, buf, cap, &n) != napi_ok) {
        buf[0] = 0;
        return -1;
    }
    return (int)n;
}

static napi_value ret_int(napi_env env, int v) {
    napi_value r;
    napi_create_int32(env, v, &r);
    return r;
}

// ---- Executable memory self-test (the same check the rewriter's output relies on) -------------------
static const char *exec_self_test(void) {
    static char result[128];
    uint32_t *page = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(result, sizeof result, "mmap failed, errno %d", errno);
        return result;
    }
    page[0] = 0x52800540;  // mov w0, #42
    page[1] = 0xd65f03c0;  // ret
    __builtin___clear_cache((char *)page, (char *)(page + 2));
    if (mprotect(page, 0x4000, PROT_READ | PROT_EXEC)) {
        snprintf(result, sizeof result, "mprotect failed, errno %d", errno);
    } else {
        int v = ((int (*)(void))page)();
        snprintf(result, sizeof result, v == 42 ? "ok (w0=42)" : "wrong result %d", v);
    }
    munmap(page, 0x4000);
    return result;
}

// ---- Data pack (P1): validation with stable error codes, env.txt, free space -----------------------
// Error codes shown on the ArkTS page and in the log. 0 = the pack is usable.
#define PACK_OK          0
#define PACK_E_DIR     (-20)   // pack folder not readable
#define PACK_E_MANIFEST (-21)  // nx2a-data.json missing or unreadable
#define PACK_E_FIELDS  (-22)   // nx2a-data.json lacks title_id/version
#define PACK_E_TARGET  (-23)   // pack target/package does not match this app
#define PACK_E_MODS    (-24)   // mods/ missing, or a patched module lacks its fixups/gates
#define PACK_E_SHADERS (-25)   // shaders.dmsv missing or not a DMSV bucket
#define PACK_E_ROMFS   (-26)   // RomFS/ missing, empty and without the .nx2a-romfs marker
#define PACK_E_FILE    (-27)   // an optional file (env.txt/settings.txt) is not readable

// Must match AppScope/app.json5 bundleName and tools/make_pack.py PACKAGE. A target=ohos pack with any other
// package is rejected (PACK_E_TARGET). The legacy id is the bundle this fork used before it became a standalone
// project: packs built then (nx2a-data.json "package": "com.nx2a.dm") stay importable.
#define NX2A_OHOS_BUNDLE        "com.nx2a.rhythm"
#define NX2A_OHOS_BUNDLE_LEGACY "com.nx2a.dm"

static int path_is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
static int path_is_file(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}
static int dir_entry_count(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    }
    closedir(d);
    return n;
}

// First "key": "value" string field of a JSON text (the manifest is small and its shape is fixed).
static int json_string(const char *json, const char *key, char *out, size_t n) {
    char pat[64];
    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

// 0 when the pack in dir can be started; otherwise the first problem's code, with a readable reason.
static int validate_pack(const char *dir, char *why, size_t why_n) {
    why[0] = 0;
    if (!path_is_dir(dir)) {
        snprintf(why, why_n, "pack folder is not readable: %s", dir);
        return PACK_E_DIR;
    }
    // P1 layout (as the ArkTS import writes it): <filesDir>/dm holds the pack, <filesDir>/RomFS the RomFS.
    // Android-style packs (<filesDir>/nx2a-data.json, mods/, shaders.dmsv) are accepted too.
    char dmdir[PATH_CAP + 32];
    snprintf(dmdir, sizeof dmdir, "%s/dm", dir);
    char p[PATH_CAP + 96], dmfile[PATH_CAP + 96];
    snprintf(dmfile, sizeof dmfile, "%s/nx2a-data.json", dmdir);
    snprintf(p, sizeof p, "%s/nx2a-data.json", path_is_file(dmfile) ? dmdir : dir);
    FILE *f = fopen(p, "rb");
    if (!f) {
        snprintf(why, why_n, "nx2a-data.json is missing");
        return PACK_E_MANIFEST;
    }
    static char json[65536];
    size_t got = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    json[got] = 0;
    char title[64] = "", version[64] = "", display[64] = "", package[128] = "", target[16] = "";
    json_string(json, "title_id", title, sizeof title);
    json_string(json, "version", version, sizeof version);
    json_string(json, "display", display, sizeof display);
    json_string(json, "package", package, sizeof package);
    json_string(json, "target", target, sizeof target);
    if (!title[0] || (!version[0] && !display[0])) {
        snprintf(why, why_n, "nx2a-data.json lacks title_id/version");
        return PACK_E_FIELDS;
    }
    if (target[0] && strcmp(target, "ohos")) {
        snprintf(why, why_n, "pack target is %s, this app needs ohos", target);
        return PACK_E_TARGET;
    }
    if (target[0] && package[0] && strcmp(package, NX2A_OHOS_BUNDLE) &&
        strcmp(package, NX2A_OHOS_BUNDLE_LEGACY)) {
        snprintf(why, why_n, "pack package is %s, this app is %s", package, NX2A_OHOS_BUNDLE);
        return PACK_E_TARGET;
    }
    if (package[0] && strcmp(package, NX2A_OHOS_BUNDLE) && strcmp(package, NX2A_OHOS_BUNDLE_LEGACY)) {
        // An Android pack (no target row): usable, but the strings inside env.txt may name Android paths; the
        // path variables are overridden below.
        nx_log(LOG_WARN, "pack: package %s (Android pack), app is %s", package, NX2A_OHOS_BUNDLE);
    }

    char modsdir[PATH_CAP + 32];
    snprintf(modsdir, sizeof modsdir, "%s/dm/mods", dir);
    if (!path_is_dir(modsdir)) snprintf(modsdir, sizeof modsdir, "%s/mods", dir);
    if (!path_is_dir(modsdir)) {
        snprintf(why, why_n, "mods/ is missing");
        return PACK_E_MODS;
    }
    const char *required[] = {"main.patched.bin", "main.fixups.tsv", "main.gates.tsv"};
    for (unsigned i = 0; i < sizeof required / sizeof required[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", modsdir, required[i]);
        if (!path_is_file(p)) {
            snprintf(why, why_n, "mods/%s is missing", required[i]);
            return PACK_E_MODS;
        }
    }
    DIR *d = opendir(modsdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l <= 12 || strcmp(e->d_name + l - 12, ".patched.bin")) continue;
            char base[256], fix[PATH_CAP + 96];
            snprintf(base, sizeof base, "%.*s", (int)(l - 12), e->d_name);
            snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", modsdir, base);
            if (!path_is_file(fix)) {
                snprintf(why, why_n, "mods/%s.fixups.tsv is missing", base);
                closedir(d);
                return PACK_E_MODS;
            }
        }
        closedir(d);
    }

    snprintf(dmfile, sizeof dmfile, "%s/shaders.dmsv", dmdir);
    snprintf(p, sizeof p, "%s/shaders.dmsv", path_is_file(dmfile) ? dmdir : dir);
    f = fopen(p, "rb");
    if (!f) {
        snprintf(why, why_n, "shaders.dmsv is missing");
        return PACK_E_SHADERS;
    }
    unsigned char hdr[12];
    size_t h = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (h != sizeof hdr || memcmp(hdr, "DMSV", 4)) {
        snprintf(why, why_n, "shaders.dmsv is not a DMSV bundle (bad header)");
        return PACK_E_SHADERS;
    }
    uint32_t ver = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    uint32_t count = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (ver != 1 && ver != 2) {
        snprintf(why, why_n, "shaders.dmsv version %u is not 1 or 2", ver);
        return PACK_E_SHADERS;
    }
    if (!count) {
        snprintf(why, why_n, "shaders.dmsv holds no shaders");
        return PACK_E_SHADERS;
    }

    char romfsdir[PATH_CAP + 32];
    snprintf(romfsdir, sizeof romfsdir, "%s/RomFS", dir);
    if (!path_is_dir(romfsdir)) snprintf(romfsdir, sizeof romfsdir, "%s/dm/RomFS", dir);
    if (!path_is_dir(romfsdir)) {
        snprintf(why, why_n, "RomFS/ is missing");
        return PACK_E_ROMFS;
    }
    char marker[PATH_CAP + 96], marker2[PATH_CAP + 96];
    snprintf(marker, sizeof marker, "%s/.nx2a-romfs", romfsdir);
    snprintf(marker2, sizeof marker2, "%s/nx2a-romfs.marker", romfsdir);
    int entries = dir_entry_count(romfsdir);
    if (!path_is_file(marker) && !path_is_file(marker2) && entries <= 0) {
        snprintf(why, why_n, "RomFS/ is empty and has no .nx2a-romfs / nx2a-romfs.marker marker");
        return PACK_E_ROMFS;
    }
    const char *optional[] = {"env.txt", "settings.txt"};
    for (unsigned i = 0; i < sizeof optional / sizeof optional[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dmdir, optional[i]);
        if (access(p, F_OK) != 0) snprintf(p, sizeof p, "%s/%s", dir, optional[i]);
        if (access(p, F_OK) == 0 && !path_is_file(p)) {
            snprintf(why, why_n, "%s is not a regular file", optional[i]);
            return PACK_E_FILE;
        }
    }
    snprintf(why, why_n, "ok: %s %s", title, version[0] ? version : display);
    return PACK_OK;
}

// env.txt: KEY=VALUE per line, # comments (as front_android.c). Read before the path overrides below.
static void env_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        nx_log(LOG_INFO, "boot: env.txt %s=%s", line, eq + 1);
    }
    fclose(f);
}

// // ---- Core log capture: the shared host logs through stderr; pump it into hilog tag "dm" (as d2r does) ----
// The C0 core (libdmcore.so) is a separate library whose LOG() writes to stderr. Without this pump those lines
// never reach hilog; with it `hilog -x -T dm` shows the "[dm] >>> nnMain" style lines.
static void *stderr_pump(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    size_t have = 0;
    ssize_t n;
    while ((n = read(fd, buf + have, sizeof buf - 1 - have)) > 0) {
        have += (size_t)n;
        buf[have] = 0;
        char *s = buf, *nl;
        while ((nl = strchr(s, '\n'))) {
            *nl = 0;
            OH_LOG_Print(LOG_APP, LOG_INFO, NX_LOG_DOMAIN, "dm", "%{public}s", s);
            s = nl + 1;
        }
        have = strlen(s);
        memmove(buf, s, have);
        if (have == sizeof buf - 1) {
            OH_LOG_Print(LOG_APP, LOG_INFO, NX_LOG_DOMAIN, "dm", "%{public}s", buf);
            have = 0;
        }
    }
    return NULL;
}

static void core_log_init(void) {
    static int inited;
    if (inited) return;
    inited = 1;
    int fds[2];
    if (pipe(fds)) {
        nx_log(LOG_WARN, "boot: stderr pipe failed, errno %d", errno);
        return;
    }
    dup2(fds[1], 2);
    close(fds[1]);
    setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t th;
    if (!pthread_create(&th, NULL, stderr_pump, (void *)(intptr_t)fds[0])) pthread_detach(th);
}

// ---- Core load: dlopen the prebuilt host core and start it with the sandbox mods directory (C0 contract) ----
// The library is staged into the HAP by tools/stage_c0_package.sh (app/entry/libs/arm64-v8a/libdmcore.so).
// A build without the staged core is still a valid P0 build: the app boots and reports that it is absent.
typedef int (*core_start_fn)(const char *);
typedef void (*core_input_fn)(uint64_t, int, int, int, int);
// Same layout as front.h's dm_touch: finger slot 0-15 and normalized [0,1) picture coordinates (top left).
typedef struct { uint32_t id; float x, y; } nx_touch;
typedef void (*core_touch_fn)(const nx_touch *, int);
// Mirrors ss_pointer.h in the core (the adapter publishes these; ArkTS only reads a copy).
typedef struct { float x, y, width, height; } nx_ptr_rect;
typedef struct { uint64_t id; nx_ptr_rect rect; unsigned flags; int button; } nx_ptr_target;
typedef struct {
    uint64_t owner, token, tutorial_buttons;
    int mode, count;
    nx_ptr_rect scroll;
    nx_ptr_target targets[192];
} nx_ptr_frame;
typedef int (*core_ptr_frame_fn)(void *);
typedef void (*core_ptr_event_fn)(uint64_t, int, float, float);
typedef int (*core_ptr_controls_fn)(void);
typedef void (*core_ptr_clear_fn)(void);
typedef uint64_t (*core_touch_hint_fn)(void);
typedef void (*core_hap_install_fn)(void (*)(float, float, uint64_t));
typedef void (*core_hap_suspend_fn)(int);
static core_input_fn g_core_input;
static core_touch_fn g_core_touch;
static core_ptr_frame_fn g_core_ptr_frame;
static core_ptr_event_fn g_core_ptr_event;
static core_ptr_controls_fn g_core_ptr_controls;
static core_ptr_clear_fn g_core_ptr_clear;
static core_touch_hint_fn g_core_touch_hint;
static core_hap_install_fn g_core_hap_install;
static core_hap_suspend_fn g_core_hap_suspend;
static uint64_t g_buttons;   // currently held buttons (OR of the on-screen pad, one press per button)
static int32_t g_axes[4];    // left x/y, right x/y, -32767..32767
// Front-end touch points pushed by ArkTS (the Touchable-style overlay). The whole set is re-sent on every change.
static nx_touch g_touch[10];
static int g_touch_n;
static int core_load(const char *mods_dir) {
    void *handle = dlopen("libdmcore.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        nx_log(LOG_WARN, "boot: libdmcore.so not staged (%s): P0 skeleton only", dlerror());
        return 1;
    }
    core_start_fn start = (core_start_fn)(void *)dlsym(handle, "dm_ohos_start");
    g_core_input = (core_input_fn)(void *)dlsym(handle, "dm_ohos_input");
    g_core_touch = (core_touch_fn)(void *)dlsym(handle, "dm_touch_set");
    if (!g_core_touch) nx_log(LOG_WARN, "boot: libdmcore.so has no dm_touch_set (touchscreen disabled)");
    g_core_ptr_frame = (core_ptr_frame_fn)(void *)dlsym(handle, "dm_ohos_pointer_frame");
    g_core_ptr_event = (core_ptr_event_fn)(void *)dlsym(handle, "dm_ohos_pointer_event");
    g_core_ptr_controls = (core_ptr_controls_fn)(void *)dlsym(handle, "dm_ohos_pointer_controls");
    g_core_ptr_clear = (core_ptr_clear_fn)(void *)dlsym(handle, "dm_ohos_pointer_clear");
    g_core_touch_hint = (core_touch_hint_fn)(void *)dlsym(handle, "dm_ohos_touch_hint");
    g_core_hap_install = (core_hap_install_fn)(void *)dlsym(handle, "dm_ohos_haptics_install");
    g_core_hap_suspend = (core_hap_suspend_fn)(void *)dlsym(handle, "dm_ohos_haptics_suspend");
    if (g_core_ptr_frame) nx_log(LOG_INFO, "boot: direct-menu pointer bridge available");
    if (!start) {
        nx_log(LOG_ERROR, "boot: libdmcore.so has no dm_ohos_start: %s", dlerror());
        return -1;
    }
    // Hand the XComponent window to the game's gfx_vk_present: libdmcore's weak dm_front_window consults this provider.
    {
        typedef void (*set_front_fn)(void *(*)(void));
        set_front_fn set_front = (set_front_fn)(void *)dlsym(handle, "dm_gfx_set_front_window_provider");
        if (set_front) { set_front((void *(*)(void))dm_ohos_front_window); nx_log(LOG_INFO, "boot: game window provider installed"); }
        else nx_log(LOG_WARN, "boot: game window provider setter missing in libdmcore.so");
    }
    nx_log(LOG_INFO, "boot: dm_ohos_start(%s)", mods_dir);
    int rc = start(mods_dir);
    nx_log(LOG_INFO, "boot: dm_ohos_start returned %d", rc);
    return 0;
}

// ---- XComponent surface: record the window and geometry; presentation attaches here in G0b ---------
static void surface_set(OH_NativeXComponent *component, void *window) {
    g_window = window;
    if (window && component) {
        OH_NativeXComponent_GetXComponentSize(component, window, &g_win_w, &g_win_h);
        snprintf(g_surface, sizeof g_surface, "surface: %llux%llu (px)", (unsigned long long)g_win_w,
                 (unsigned long long)g_win_h);
        nx_log(LOG_INFO, "boot: XComponent surface ready %llux%llu", (unsigned long long)g_win_w,
               (unsigned long long)g_win_h);
        dm_gfx_surface_set(window, (int)g_win_w, (int)g_win_h);
    } else {
        snprintf(g_surface, sizeof g_surface, "surface: detached");
        nx_log(LOG_INFO, "boot: XComponent surface detached");
        dm_gfx_surface_set(NULL, 0, 0);
    }
}

static void on_created(OH_NativeXComponent *c, void *window) { surface_set(c, window); }
static void on_changed(OH_NativeXComponent *c, void *window) { surface_set(c, window); }
static void on_destroyed(OH_NativeXComponent *c, void *window) {
    (void)c;
    (void)window;
    surface_set(NULL, NULL);
}
static void on_touch(OH_NativeXComponent *c, void *window) {
    (void)c;
    (void)window;
}
static OH_NativeXComponent_Callback g_xcb = {on_created, on_changed, on_destroyed, on_touch};

// Debug: base64-log the runtime-captured shaders (files/dm/captured_shaders/*.bin) to hilog, because the app sandbox
// cannot be read with hdc. The host rebuilds the files from the CAPFILE/CAPDATA lines and feeds shader_vk.py --capture.
static const char B64C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_line(const char *name, int off, const unsigned char *p, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n && o + 8 < cap; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (unsigned)(i + 1 < n ? p[i + 1] : 0) << 8 | (unsigned)(i + 2 < n ? p[i + 2] : 0);
        out[o++] = B64C[(v >> 18) & 63]; out[o++] = B64C[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64C[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64C[v & 63] : '=';
    }
    out[o] = 0;
    nx_log(LOG_INFO, "CAPDATA %s %d %s", name, off, out);
}
static napi_value js_clear_captures(napi_env env, napi_callback_info info) {
    (void)env; (void)info;
    char dir[PATH_CAP]; snprintf(dir, sizeof dir, "%s/dm/captured_shaders", g_files);
    DIR *d = opendir(dir); if (!d) return NULL;
    struct dirent *e; char path[PATH_CAP + 256];
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(path, sizeof path, "%s/%s", dir, e->d_name); unlink(path); }
    closedir(d); nx_log(LOG_INFO, "CAPRESET %s", dir); return NULL;
}

static napi_value js_dump_captures(napi_env env, napi_callback_info info) {
    (void)env; (void)info;
    char dir[PATH_CAP]; snprintf(dir, sizeof dir, "%s/dm/captured_shaders", g_files);
    DIR *d = opendir(dir); if (!d) { nx_log(LOG_WARN, "CAPDONE 0 (cannot open %s)", dir); return NULL; }
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".part")) continue;
        char path[PATH_CAP + 256]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > 4 * 1024 * 1024) { fclose(f); continue; }
        unsigned char *buf = malloc((size_t)len); if (!buf) { fclose(f); continue; }
        if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); continue; }
        fclose(f);
        nx_log(LOG_INFO, "CAPFILE %s %ld", e->d_name, len);
        char line[1800];
        for (int off = 0; off < (int)len; off += 480) b64_line(e->d_name, off, buf + off, (int)((len - off) < 480 ? (len - off) : 480), line, sizeof line);
        free(buf); n++;
    }
    closedir(d);
    nx_log(LOG_INFO, "CAPDONE %d", n);
    return NULL;
}

// press(buttons, down): the first OHOS input bridge; ArkTS maps a screen touch to the A button.
static napi_value js_press(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int64_t buttons = 0;
    bool down = true;
    if (argc > 0) napi_get_value_int64(env, argv[0], &buttons);
    if (argc > 1) napi_get_value_bool(env, argv[1], &down);
    uint64_t mask = (uint64_t)buttons;
    if (down) g_buttons |= mask; else g_buttons &= ~mask;
    if (g_core_input) g_core_input(g_buttons, g_axes[0], g_axes[1], g_axes[2], g_axes[3]);
    return ret_int(env, 0);
}

// touch(id, x, y, down): one finger of the ArkUI touch overlay in normalized [0,1) picture coordinates (top left).
// The overlay keeps the full finger set: down/move add or update a slot, up removes it, and the host receives all
// points on every change (dm_touch_set). This is the same touchscreen path Touchable's menus use.
static napi_value js_touch(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int32_t id = 0;
    bool down = true;
    double x = 0, y = 0;
    if (argc > 0) { int64_t t = 0; napi_get_value_int64(env, argv[0], &t); id = (int32_t)t; }
    if (argc > 1) napi_get_value_double(env, argv[1], &x);
    if (argc > 2) napi_get_value_double(env, argv[2], &y);
    if (argc > 3) napi_get_value_bool(env, argv[3], &down);
    if (id < 0 || id >= 16) return ret_int(env, -1);
    if (x < 0) x = 0; if (x > 0.999f) x = 0.999f;
    if (y < 0) y = 0; if (y > 0.999f) y = 0.999f;
    int slot = -1;
    for (int i = 0; i < g_touch_n; i++) if ((int)g_touch[i].id == id) { slot = i; break; }
    if (down) {
        if (slot < 0) {
            if (g_touch_n >= (int)(sizeof g_touch / sizeof g_touch[0])) return ret_int(env, -1);
            slot = g_touch_n++;
            g_touch[slot].id = (uint32_t)id;
        }
        g_touch[slot].x = (float)x;
        g_touch[slot].y = (float)y;
    } else if (slot >= 0) {
        g_touch[slot] = g_touch[--g_touch_n];
    }
    if (g_core_touch) g_core_touch(g_touch, g_touch_n);
    return ret_int(env, g_touch_n);
}

// touchClear(): lift every finger (page hidden / app backgrounded).
static napi_value js_touch_clear(napi_env env, napi_callback_info info) {
    (void)info;
    g_touch_n = 0;
    if (g_core_touch) g_core_touch(NULL, 0);
    return ret_int(env, 0);
}

// pointerFrame(): the menu adapter's current frame as JSON. mode 1 = MENU, 2 = MAP, 3 = HINTS, 4 = WAIT,
// 5 = CUTSCENE; coordinates are 1920x1080 game pixels. Empty string when the bridge is absent.
static napi_value js_pointer_frame(napi_env env, napi_callback_info info) {
    (void)info;
    if (!g_core_ptr_frame) {
        napi_value empty;
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &empty);
        return empty;
    }
    static nx_ptr_frame f;
    int valid = g_core_ptr_frame(&f);
    static char out[65536];
    if (!valid || f.mode <= 0 || f.count < 0) {
        snprintf(out, sizeof out, "");
    } else {
        static uint64_t last_token; static int last_mode = -1, last_count = -1;
        if (f.token != last_token || f.mode != last_mode || f.count != last_count) {
            nx_log(LOG_INFO, "MARK pointer: token=%llu mode=%d count=%d tutorial=%llu", (unsigned long long)f.token, f.mode, f.count,
                   (unsigned long long)f.tutorial_buttons);
            last_token = f.token; last_mode = f.mode; last_count = f.count;
        }
        int at = snprintf(out, sizeof out,
                          "{\"mode\":%d,\"token\":%llu,\"tutorial\":%llu,\"count\":%d,\"targets\":[",
                          f.mode, (unsigned long long)f.token, (unsigned long long)f.tutorial_buttons,
                          f.count > 192 ? 192 : f.count);
        for (int i = 0; i < f.count && i < 192 && at < (int)sizeof out - 160; i++) {
            const nx_ptr_target *t = &f.targets[i];
            at += snprintf(out + at, sizeof out - (size_t)at,
                           "%s{\"id\":%llu,\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,\"f\":%u,\"b\":%d}",
                           i ? "," : "", (unsigned long long)t->id, t->rect.x, t->rect.y,
                           t->rect.width, t->rect.height, t->flags, t->button);
        }
        if (at < (int)sizeof out - 2) { out[at++] = ']'; out[at++] = '}'; out[at] = 0; }
    }
    napi_value v;
    napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &v);
    return v;
}

// pointerEvent(token, type, x, y): type 0 down, 1 move, 2 up, 3 cancel. x/y are 1920x1080 game pixels.
static napi_value js_pointer_event(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int64_t token = 0, type = 0;
    double x = 0, y = 0;
    if (argc > 0) napi_get_value_int64(env, argv[0], &token);
    if (argc > 1) napi_get_value_int64(env, argv[1], &type);
    if (argc > 2) napi_get_value_double(env, argv[2], &x);
    if (argc > 3) napi_get_value_double(env, argv[3], &y);
    if (g_core_ptr_event) g_core_ptr_event((uint64_t)token, (int)type, (float)x, (float)y);
    return ret_int(env, 0);
}

// touchHint(): packed Touchable hint: layout | first << 8 | second << 16 | extra << 24.
static napi_value js_touch_hint(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value v;
    napi_create_double(env, (double)(g_core_touch_hint ? g_core_touch_hint() : 0), &v);
    return v;
}

static napi_value js_pointer_controls(napi_env env, napi_callback_info info) {
    (void)info;
    return ret_int(env, g_core_ptr_controls ? g_core_ptr_controls() : 0);
}

static napi_value js_pointer_clear(napi_env env, napi_callback_info info) {
    (void)info;
    if (g_core_ptr_clear) g_core_ptr_clear();
    return ret_int(env, 0);
}

// stick(lx, ly, rx, ry): analog sticks in -32767..32767 (screen up/right positive; ArkTS computes them).
static napi_value js_stick(napi_env env, napi_callback_info info) {
    size_t argc = 4; napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    for (int i = 0; i < 4; i++) {
        int32_t v = 0;
        if (i < (int)argc) { int64_t t = 0; napi_get_value_int64(env, argv[i], &t); v = (int32_t)t; }
        if (v > 32767) v = 32767; if (v < -32767) v = -32767;
        g_axes[i] = v;
    }
    if (g_core_input) g_core_input(g_buttons, g_axes[0], g_axes[1], g_axes[2], g_axes[3]);
    return ret_int(env, 0);
}

// ---- start(filesDir, lang): sandbox directories, DM_* environment, boot log ------------------------
static napi_value js_start(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char lang[64] = {0};
    if (argc < 1 || get_str(env, argv[0], g_files, sizeof g_files) <= 0) return ret_int(env, -1);
    if (argc > 1) get_str(env, argv[1], lang, sizeof lang);
    if (g_started) return ret_int(env, 0);

    snprintf(g_runtime, sizeof g_runtime, "%s/dm", g_files);
    snprintf(g_romfs, sizeof g_romfs, "%s/RomFS", g_files);
    snprintf(g_save, sizeof g_save, "%s/save", g_files);
    snprintf(g_temp, sizeof g_temp, "%s/temp", g_files);
    mkdirs(g_files);
    mkdirs(g_runtime);
    mkdirs(g_romfs);
    mkdirs(g_save);
    mkdirs(g_temp);

    char log_path[PATH_CAP + 32];
    snprintf(log_path, sizeof log_path, "%s/boot.log", g_runtime);
    g_logf = fopen(log_path, "a");
    g_bootinfo[0] = 0;

    nx_log(LOG_INFO, "nx2a-ohos boot: nx2android HarmonyOS NEXT platform skeleton (P0)");
    boot_line("nx2a-ohos boot: nx2android HarmonyOS NEXT platform skeleton (P0)");
    boot_line("files dir %s", g_files);
    boot_line("DM_ROOT=%s", g_files);
    boot_line("DM_ROMFS=%s", g_romfs);
    boot_line("DM_SAVE=%s", g_save);
    boot_line("DM_TEMP=%s", g_temp);
    boot_line("log file %s", log_path);
    boot_line("page size %ld", (long)sysconf(_SC_PAGESIZE));
    boot_line("executable memory self-test: %s", exec_self_test());

    // The XComponent is already a landscape window on this panel; let the OHOS compositor handle the remaining
    // rotation instead of letting gfx_vk_present pre-rotate a second time (which squashed the 16:9 game into a band).
    setenv("DM_VK_PRETRANSFORM", "0", 0);
    setenv("DM_VK_SHOT", "100,1000,3000,6000", 0);   // debug: sample the presented window texture at several frames
    setenv("DM_ROOT", g_runtime, 1);
    setenv("DM_ROMFS", g_romfs, 1);
    setenv("DM_SAVE", g_save, 1);
    setenv("DM_TEMP", g_temp, 1);
    // The data pack's env.txt may pin DM_LANG. The app asks for the system language; apply it after
    // env.txt below so a pack cannot force English on a Chinese system.

    // A data pack imported by the ArkTS page keeps its runtime files in <filesDir>/dm (mods, shaders.dmsv,
    // env.txt, settings.txt, aoc, sysdata, fonts, nx2a-data.json) and its RomFS in <filesDir>/RomFS. env.txt is
    // applied first, then the variables that name sandbox paths are overridden, because an Android pack's
    // env.txt names Android paths.
    char p[PATH_CAP + 96];
    snprintf(p, sizeof p, "%s/env.txt", g_runtime);
    env_file(p);
    if (path_is_file(p)) boot_line("env.txt applied");
    if (lang[0]) {
        setenv("DM_LANG", lang, 1);
        boot_line("DM_LANG=%s (system language)", lang);
    }
    setenv("DM_ROMFS", g_romfs, 1);
    snprintf(p, sizeof p, "%s/shaders.dmsv", g_runtime);
    if (path_is_file(p)) { setenv("DM_VK_SHADERS", p, 1); boot_line("DM_VK_SHADERS=%s", p); }
    snprintf(p, sizeof p, "%s/aoc", g_runtime);
    if (path_is_dir(p)) { setenv("DM_AOC_DIR", p, 1); boot_line("DM_AOC_DIR=%s", p); }
    snprintf(p, sizeof p, "%s/sysdata", g_runtime);
    if (path_is_dir(p)) { setenv("DM_SYSDATA", p, 1); boot_line("DM_SYSDATA=%s", p); }
    snprintf(p, sizeof p, "%s/standard.ttf", g_runtime);
    if (path_is_file(p)) { setenv("DM_SHARED_FONT", p, 1); boot_line("DM_SHARED_FONT=%s", p); }
    snprintf(p, sizeof p, "%s/nintendo_ext.ttf", g_runtime);
    if (path_is_file(p)) { setenv("DM_SHARED_FONT_EXT", p, 1); boot_line("DM_SHARED_FONT_EXT=%s", p); }

    // CPU top frequencies: the big cores are where the translation thread will be pinned (as in d2r).
    char line[768] = {0};
    int at = 0;
    for (int c = 0; c < 16; c++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1) {
            at += snprintf(line + at, sizeof line - (size_t)at, " %d:%ld", c, khz / 1000);
        }
        fclose(f);
        if (at > (int)sizeof line - 32) break;
    }
    boot_line("cpu max MHz%s", at ? line : " (unavailable)");
    if (chdir(g_files)) nx_log(LOG_WARN, "boot: chdir(%s) failed, errno %d", g_files, errno);

    // Modules live in <filesDir>/dm/mods for both an imported pack and the C0 development staging.
    char mods_dir[PATH_CAP + 16];
    snprintf(mods_dir, sizeof mods_dir, "%s/mods", g_runtime);
    mkdirs(mods_dir);
    boot_line("mods dir %s", mods_dir);
    boot_line("P0 skeleton ready: directories and environment set");

    // C0: capture the shared host's stderr into hilog, then start it (no-op when the core is not staged).
    const char *no_core = getenv("DM_OHOS_NO_CORE");
    if (no_core && atoi(no_core)) {
        boot_line("core start skipped (DM_OHOS_NO_CORE)");
    } else {
        core_log_init();
        boot_line("core load: %d", core_load(mods_dir));
    }

    g_started = 1;
    return ret_int(env, 0);
}

// ---- bootInfo(): the same lines for the ArkTS page ------------------------------------------------
static napi_value js_boot_info(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value out;
    napi_create_string_utf8(env, g_bootinfo, NAPI_AUTO_LENGTH, &out);
    return out;
}

static napi_value js_surface_info(napi_env env, napi_callback_info info) {
    (void)info;
    const char *s = g_surface[0] ? g_surface : "surface: waiting for XComponent";
    napi_value out;
    napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &out);
    return out;
}

static napi_value js_exec_probe(napi_env env, napi_callback_info info) {
    (void)info;
    const char *r = exec_self_test();
    napi_value out;
    napi_create_string_utf8(env, r, NAPI_AUTO_LENGTH, &out);
    return out;
}

// ---- validatePack(dir) / freeSpace(path): used by the ArkTS import page ----------------------------
static napi_value js_validate_pack(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char dir[PATH_CAP] = {0};
    if (argc < 1 || get_str(env, argv[0], dir, sizeof dir) <= 0) {
        napi_value bad;
        napi_create_string_utf8(env, "-20:no pack folder given", NAPI_AUTO_LENGTH, &bad);
        return bad;
    }
    char why[512];
    int rc = validate_pack(dir, why, sizeof why);
    nx_log(rc == 0 ? LOG_INFO : LOG_ERROR, "pack validate: %d %s", rc, why);
    char out[600];
    snprintf(out, sizeof out, "%d:%s", rc, why);
    napi_value v;
    napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &v);
    return v;
}

static napi_value js_free_space(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char path[PATH_CAP] = {0};
    if (argc < 1 || get_str(env, argv[0], path, sizeof path) <= 0) snprintf(path, sizeof path, "%s", g_files);
    struct statvfs st;
    double free_bytes = -1;
    if (!statvfs(path[0] ? path : ".", &st)) free_bytes = (double)st.f_bavail * (double)st.f_frsize;
    napi_value v;
    napi_create_double(env, free_bytes, &v);
    return v;
}


// ---- haptics: native state -> ArkTS vibrator player ------------------------------------------------
// Core haptics has no dynamic-player API like Core Haptics; ArkTS receives intensity/sharpness and starts
// one continuous pattern at a time. The native side still keeps the exact Touchable state model.
static napi_threadsafe_function g_hap_tsfn;

static void hap_js_call(napi_env env, napi_value js_cb, void *context, void *data) {
    (void)context;
    double *v = (double *)data;
    if (!env || !js_cb || !v) { free(v); return; }
    napi_value argv[3];
    napi_create_double(env, v[0], &argv[0]);
    napi_create_double(env, v[1], &argv[1]);
    napi_create_double(env, v[2], &argv[2]);
    napi_call_function(env, NULL, js_cb, 3, argv, NULL);
    free(v);
}
static void hap_sink(float intensity, float sharpness, uint64_t epoch) {
    if (!g_hap_tsfn) return;
    double *v = (double *)malloc(3 * sizeof(double));
    if (!v) return;
    v[0] = intensity; v[1] = sharpness; v[2] = (double)epoch;
    if (napi_call_threadsafe_function(g_hap_tsfn, v, napi_tsfn_nonblocking) != napi_ok) free(v);
}
static napi_value js_haptics_start(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    napi_valuetype type = napi_undefined;
    if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_function) return ret_int(env, -1);
    if (g_hap_tsfn) { napi_release_threadsafe_function(g_hap_tsfn, napi_tsfn_release); g_hap_tsfn = NULL; }
    napi_value name;
    napi_create_string_utf8(env, "dm.haptics", NAPI_AUTO_LENGTH, &name);
    napi_status st = napi_create_threadsafe_function(env, argv[0], NULL, name, 0, 1, NULL, NULL, NULL, hap_js_call, &g_hap_tsfn);
    if (st != napi_ok) { g_hap_tsfn = NULL; nx_log(LOG_ERROR, "haptics: cannot create the JS callback: %d", (int)st); return ret_int(env, -2); }
    if (!g_core_hap_install) { nx_log(LOG_WARN, "haptics: libdmcore.so has no haptics sink"); return ret_int(env, -3); }
    g_core_hap_install(hap_sink);
    nx_log(LOG_INFO, "haptics: ArkTS vibrator player attached");
    return ret_int(env, 0);
}
static napi_value js_haptics_stop(napi_env env, napi_callback_info info) {
    (void)info;
    if (g_hap_tsfn) { napi_release_threadsafe_function(g_hap_tsfn, napi_tsfn_release); g_hap_tsfn = NULL; }
    return ret_int(env, 0);
}
static napi_value js_haptics_suspend(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1]; bool on = false;
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    if (g_core_hap_suspend) g_core_hap_suspend(on ? 1 : 0);
    return ret_int(env, 0);
}
// ---- Data pack ZIP import: extract straight from the picker file descriptor (no zip copy into the sandbox) ---
// ArkTS opens the picked .zip with fileIo.openSync(uri) and passes the fd here; the archive is read with pread()
// and written to outDir on a background thread, while ArkTS polls unzipPoll() for progress.
//   zipInfo(fd)          -> "rc:unpackedBytes:entries:reason" (rc 0 = OK)
//   unzipStart(fd, dir)  -> "0" started, or "-<code>:reason"
//   unzipPoll()          -> "state:rc:done:total:reason"; state 0 idle, 1 running, 2 done, 3 failed.
//                           The terminal state is consumed when reported (returns to idle).
extern int uf_zip_info(int fd, uint64_t *unpacked, uint32_t *entries, char *why, size_t why_n);
extern int uf_extract(int fd, const char *out_dir, volatile uint64_t *done, uint64_t total, char *why, size_t why_n);

static pthread_mutex_t g_unzip_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_unzip_state = 0;   // 0 idle, 1 running, 2 done, 3 failed
static int g_unzip_rc = 0;
static uint64_t g_unzip_total = 0;
static char g_unzip_why[512];
static char g_unzip_dir[PATH_CAP];
static int g_unzip_fd = -1;
static uint64_t g_unzip_done = 0;   // written by the worker through __atomic_add_fetch

static void *unzip_thread(void *arg) {
    (void)arg;
    char why[512] = {0};
    int rc = uf_extract(g_unzip_fd, g_unzip_dir, &g_unzip_done, g_unzip_total, why, sizeof why);
    pthread_mutex_lock(&g_unzip_lock);
    g_unzip_rc = rc;
    g_unzip_state = rc == 0 ? 2 : 3;
    snprintf(g_unzip_why, sizeof g_unzip_why, "%s", why);
    pthread_mutex_unlock(&g_unzip_lock);
    return NULL;
}

static napi_value js_zip_info(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int32_t fd = -1;
    char out[800];
    if (argc < 1 || napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        snprintf(out, sizeof out, "-1:0:0:no file descriptor");
    } else {
        uint64_t unpacked = 0;
        uint32_t entries = 0;
        char why[512] = {0};
        int rc = uf_zip_info(fd, &unpacked, &entries, why, sizeof why);
        snprintf(out, sizeof out, "%d:%llu:%u:%s", rc, (unsigned long long)unpacked, entries, why);
        nx_log(LOG_INFO, "unzip info: %s", out);
    }
    napi_value v;
    napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &v);
    return v;
}

static napi_value js_unzip_start(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int32_t fd = -1;
    char dir[PATH_CAP] = {0};
    char out[640];
    if (argc < 2 || napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0 ||
        get_str(env, argv[1], dir, sizeof dir) <= 0 || dir[0] != '/') {
        snprintf(out, sizeof out, "-1:bad arguments");
    } else {
        pthread_mutex_lock(&g_unzip_lock);
        int busy = g_unzip_state == 1;
        if (!busy) {
            g_unzip_fd = fd;
            snprintf(g_unzip_dir, sizeof g_unzip_dir, "%s", dir);
            g_unzip_done = 0;
            g_unzip_total = 0;
            g_unzip_rc = 0;
            g_unzip_why[0] = 0;
            g_unzip_state = 1;
        }
        pthread_mutex_unlock(&g_unzip_lock);
        if (busy) {
            snprintf(out, sizeof out, "-2:another import is running");
        } else {
            char why[512] = {0};
            uint64_t unpacked = 0;
            uint32_t entries = 0;
            int irc = uf_zip_info(g_unzip_fd, &unpacked, &entries, why, sizeof why);
            g_unzip_total = unpacked;
            pthread_t th;
            if (irc != 0) {
                pthread_mutex_lock(&g_unzip_lock);
                g_unzip_state = 3;
                g_unzip_rc = irc;
                snprintf(g_unzip_why, sizeof g_unzip_why, "%s", why);
                pthread_mutex_unlock(&g_unzip_lock);
                snprintf(out, sizeof out, "%d:%s", irc, why);
            } else if (pthread_create(&th, NULL, unzip_thread, NULL) != 0) {
                pthread_mutex_lock(&g_unzip_lock);
                g_unzip_state = 3;
                g_unzip_rc = -1;
                snprintf(g_unzip_why, sizeof g_unzip_why, "cannot start the extraction thread");
                pthread_mutex_unlock(&g_unzip_lock);
                snprintf(out, sizeof out, "-1:cannot start the extraction thread");
            } else {
                pthread_detach(th);
                snprintf(out, sizeof out, "0");
            }
        }
    }
    napi_value v;
    napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &v);
    return v;
}

static napi_value js_unzip_poll(napi_env env, napi_callback_info info) {
    (void)info;
    char out[900];
    pthread_mutex_lock(&g_unzip_lock);
    uint64_t done = __atomic_load_n(&g_unzip_done, __ATOMIC_RELAXED);
    int state = g_unzip_state;
    int rc = g_unzip_rc;
    uint64_t total = g_unzip_total;
    snprintf(out, sizeof out, "%d:%d:%llu:%llu:%s", state, rc, (unsigned long long)done,
             (unsigned long long)total, g_unzip_why);
    if (state >= 2) g_unzip_state = 0;   // the terminal state is consumed once
    pthread_mutex_unlock(&g_unzip_lock);
    napi_value v;
    napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &v);
    return v;
}

static napi_value init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"start", NULL, js_start, NULL, NULL, NULL, napi_default, NULL},
        {"bootInfo", NULL, js_boot_info, NULL, NULL, NULL, napi_default, NULL},
        {"surfaceInfo", NULL, js_surface_info, NULL, NULL, NULL, napi_default, NULL},
        {"press", NULL, js_press, NULL, NULL, NULL, napi_default, NULL},
        {"stick", NULL, js_stick, NULL, NULL, NULL, napi_default, NULL},
        {"touch", NULL, js_touch, NULL, NULL, NULL, napi_default, NULL},
        {"touchClear", NULL, js_touch_clear, NULL, NULL, NULL, napi_default, NULL},
        {"pointerFrame", NULL, js_pointer_frame, NULL, NULL, NULL, napi_default, NULL},
        {"pointerEvent", NULL, js_pointer_event, NULL, NULL, NULL, napi_default, NULL},
        {"pointerControls", NULL, js_pointer_controls, NULL, NULL, NULL, napi_default, NULL},
        {"touchHint", NULL, js_touch_hint, NULL, NULL, NULL, napi_default, NULL},
        {"pointerClear", NULL, js_pointer_clear, NULL, NULL, NULL, napi_default, NULL},
        {"dumpCaptures", NULL, js_dump_captures, NULL, NULL, NULL, napi_default, NULL},
        {"clearCaptures", NULL, js_clear_captures, NULL, NULL, NULL, napi_default, NULL},
        {"execProbe", NULL, js_exec_probe, NULL, NULL, NULL, napi_default, NULL},
        {"validatePack", NULL, js_validate_pack, NULL, NULL, NULL, napi_default, NULL},
        {"freeSpace", NULL, js_free_space, NULL, NULL, NULL, napi_default, NULL},
        {"zipInfo", NULL, js_zip_info, NULL, NULL, NULL, napi_default, NULL},
        {"unzipStart", NULL, js_unzip_start, NULL, NULL, NULL, napi_default, NULL},
        {"unzipPoll", NULL, js_unzip_poll, NULL, NULL, NULL, napi_default, NULL},
        {"hapticsStart", NULL, js_haptics_start, NULL, NULL, NULL, napi_default, NULL},
        {"hapticsStop", NULL, js_haptics_stop, NULL, NULL, NULL, napi_default, NULL},
        {"hapticsSuspend", NULL, js_haptics_suspend, NULL, NULL, NULL, napi_default, NULL},
    };
    napi_define_properties(env, exports, sizeof desc / sizeof desc[0], desc);

    // When an XComponent loads this library, exports carries the native component handle.
    napi_value xobj;
    OH_NativeXComponent *xc = NULL;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xobj) == napi_ok &&
        napi_unwrap(env, xobj, (void **)&xc) == napi_ok && xc) {
        OH_NativeXComponent_RegisterCallback(xc, &g_xcb);
    }
    return exports;
}

static napi_module g_module = {1, 0, NULL, init, "dmhost", NULL, {0}};

__attribute__((constructor)) static void register_module(void) { napi_module_register(&g_module); }
