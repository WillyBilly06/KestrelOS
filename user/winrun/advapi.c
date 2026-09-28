/* advapi.c - the registry, and the few advapi32 calls beside it.
 *
 * The registry is where a Windows program keeps everything it wants to survive
 * being closed: where it was installed, what the user chose last time, whether
 * it has been run before.  A program that cannot write one either refuses to
 * install or forgets everything, so this is not an optional corner.
 *
 * What it is here is a flat list of values, each with the full path of the key
 * it belongs to, kept in a file and written back whenever it changes.  A tree
 * would be the obvious shape and is not worth it: real registry use is almost
 * entirely "open one key, read three values", which a list answers as fast.
 */
#include "win.h"

#define HKEY_CLASSES_ROOT   ((HANDLE)(uintptr_t)0x80000000ULL)
#define HKEY_CURRENT_USER   ((HANDLE)(uintptr_t)0x80000001ULL)
#define HKEY_LOCAL_MACHINE  ((HANDLE)(uintptr_t)0x80000002ULL)
#define HKEY_USERS          ((HANDLE)(uintptr_t)0x80000003ULL)

#define REG_NONE      0
#define REG_SZ        1
#define REG_EXPAND_SZ 2
#define REG_BINARY    3
#define REG_DWORD     4
#define REG_QWORD     11

#define REGISTRY_FILE "/etc/winreg"
#define MAX_VALUES    256
#define MAX_OPEN_KEYS 32

typedef struct {
    bool  used;
    char  path[192];
    char  name[64];
    DWORD type;
    DWORD length;
    BYTE  data[256];
} regvalue;

typedef struct { bool used; char path[192]; } openkey;

static regvalue values[MAX_VALUES];
static openkey  keys[MAX_OPEN_KEYS];
static bool     registry_loaded;
static bool     registry_dirty;

#define KEY_BASE 0x5000

/* ---------------------------------------------------------------- the file */

/* One line per value:  path|name|type|hex-encoded data
 * Hex rather than raw because a value can hold anything, newlines included. */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void registry_load(void) {
    if (registry_loaded) return;
    registry_loaded = true;

    static char text[16384];
    ssize_t n = read_file(REGISTRY_FILE, text, sizeof text - 1);
    if (n <= 0) return;
    text[n] = 0;

    char *line = text;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;

        char *bar1 = strchr(line, '|');
        if (!bar1) goto next;
        *bar1 = 0;
        char *bar2 = strchr(bar1 + 1, '|');
        if (!bar2) goto next;
        *bar2 = 0;
        char *bar3 = strchr(bar2 + 1, '|');
        if (!bar3) goto next;
        *bar3 = 0;

        for (int i = 0; i < MAX_VALUES; i++) {
            if (values[i].used) continue;
            values[i].used = true;
            strlcpy(values[i].path, line, sizeof values[i].path);
            strlcpy(values[i].name, bar1 + 1, sizeof values[i].name);
            values[i].type = (DWORD)strtoul(bar2 + 1, NULL, 10);
            const char *hex = bar3 + 1;
            DWORD len = 0;
            while (hex[0] && hex[1] && len < sizeof values[i].data) {
                int hi = hexval(hex[0]), lo = hexval(hex[1]);
                if (hi < 0 || lo < 0) break;
                values[i].data[len++] = (BYTE)((hi << 4) | lo);
                hex += 2;
            }
            values[i].length = len;
            break;
        }
    next:
        line = end ? end + 1 : NULL;
    }
}

static void registry_save(void) {
    if (!registry_dirty) return;
    static char text[16384];
    size_t n = 0;
    for (int i = 0; i < MAX_VALUES && n < sizeof text - 600; i++) {
        if (!values[i].used) continue;
        n += (size_t)snprintf(text + n, sizeof text - n, "%s|%s|%u|",
                              values[i].path, values[i].name, values[i].type);
        for (DWORD k = 0; k < values[i].length && n < sizeof text - 4; k++)
            n += (size_t)snprintf(text + n, sizeof text - n, "%02x", values[i].data[k]);
        if (n < sizeof text - 1) text[n++] = '\n';
    }
    if (write_file(REGISTRY_FILE, text, n) == 0) registry_dirty = false;
}

/* ------------------------------------------------------------------ paths */

static const char *root_name(HANDLE h) {
    if (h == HKEY_CLASSES_ROOT) return "HKCR";
    if (h == HKEY_CURRENT_USER) return "HKCU";
    if (h == HKEY_LOCAL_MACHINE) return "HKLM";
    if (h == HKEY_USERS) return "HKU";
    return NULL;
}

static openkey *key_from(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (v < KEY_BASE || v >= KEY_BASE + MAX_OPEN_KEYS) return NULL;
    return keys[v - KEY_BASE].used ? &keys[v - KEY_BASE] : NULL;
}

/* The full path of a key, whether it was named by a root or by an open one. */
static bool full_path(HANDLE parent, const char *sub, char *out, size_t cap) {
    const char *root = root_name(parent);
    if (root) {
        if (sub && *sub) snprintf(out, cap, "%s\\%s", root, sub);
        else strlcpy(out, root, cap);
        return true;
    }
    openkey *k = key_from(parent);
    if (!k) return false;
    if (sub && *sub) snprintf(out, cap, "%s\\%s", k->path, sub);
    else strlcpy(out, k->path, cap);
    return true;
}

static HANDLE open_path(const char *path) {
    for (int i = 0; i < MAX_OPEN_KEYS; i++) {
        if (keys[i].used) continue;
        keys[i].used = true;
        strlcpy(keys[i].path, path, sizeof keys[i].path);
        return (HANDLE)(uintptr_t)(KEY_BASE + i);
    }
    return NULL;
}

static regvalue *find_value(const char *path, const char *name) {
    for (int i = 0; i < MAX_VALUES; i++) {
        if (!values[i].used) continue;
        if (strcasecmp(values[i].path, path)) continue;
        if (strcasecmp(values[i].name, name ? name : "")) continue;
        return &values[i];
    }
    return NULL;
}

/* A key exists when something is stored under it or below it, which is what a
 * flat list can answer without keeping the keys themselves. */
static bool key_exists(const char *path) {
    size_t n = strlen(path);
    for (int i = 0; i < MAX_VALUES; i++) {
        if (!values[i].used) continue;
        if (!strncasecmp(values[i].path, path, n) &&
            (values[i].path[n] == 0 || values[i].path[n] == '\\')) return true;
    }
    return false;
}

/* --------------------------------------------------------------- the calls */

static LONG WINAPI w_RegOpenKeyExA(HANDLE parent, const char *sub, DWORD options,
                                   DWORD access, HANDLE *out) {
    (void)options; (void)access;
    registry_load();
    char path[192];
    if (!full_path(parent, sub, path, sizeof path)) return ERROR_INVALID_HANDLE;
    if (!key_exists(path)) return ERROR_FILE_NOT_FOUND;
    HANDLE h = open_path(path);
    if (!h) return ERROR_NOT_ENOUGH_MEMORY;
    if (out) *out = h;
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegCreateKeyExA(HANDLE parent, const char *sub, DWORD reserved,
                                     char *cls, DWORD options, DWORD access,
                                     void *sa, HANDLE *out, DWORD *disposition) {
    (void)reserved; (void)cls; (void)options; (void)access; (void)sa;
    registry_load();
    char path[192];
    if (!full_path(parent, sub, path, sizeof path)) return ERROR_INVALID_HANDLE;
    bool existed = key_exists(path);
    HANDLE h = open_path(path);
    if (!h) return ERROR_NOT_ENOUGH_MEMORY;
    if (out) *out = h;
    if (disposition) *disposition = existed ? 2 /* OPENED */ : 1 /* CREATED */;
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegCloseKey(HANDLE h) {
    openkey *k = key_from(h);
    if (!k) return root_name(h) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
    k->used = false;
    registry_save();
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegQueryValueExA(HANDLE h, const char *name, DWORD *reserved,
                                      DWORD *type, BYTE *data, DWORD *len) {
    (void)reserved;
    registry_load();
    char path[192];
    if (!full_path(h, NULL, path, sizeof path)) return ERROR_INVALID_HANDLE;
    regvalue *v = find_value(path, name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    if (type) *type = v->type;
    if (!data) { if (len) *len = v->length; return ERROR_SUCCESS; }
    if (!len) return ERROR_INVALID_PARAMETER;
    if (*len < v->length) { *len = v->length; return 234 /* ERROR_MORE_DATA */; }
    memcpy(data, v->data, v->length);
    *len = v->length;
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegSetValueExA(HANDLE h, const char *name, DWORD reserved,
                                    DWORD type, const BYTE *data, DWORD len) {
    (void)reserved;
    registry_load();
    char path[192];
    if (!full_path(h, NULL, path, sizeof path)) return ERROR_INVALID_HANDLE;
    if (len > sizeof values[0].data) return ERROR_NOT_ENOUGH_MEMORY;

    regvalue *v = find_value(path, name);
    if (!v) {
        for (int i = 0; i < MAX_VALUES; i++) {
            if (values[i].used) continue;
            v = &values[i];
            v->used = true;
            strlcpy(v->path, path, sizeof v->path);
            strlcpy(v->name, name ? name : "", sizeof v->name);
            break;
        }
    }
    if (!v) return ERROR_NOT_ENOUGH_MEMORY;
    v->type = type;
    v->length = len;
    if (data && len) memcpy(v->data, data, len);
    registry_dirty = true;
    registry_save();
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegDeleteValueA(HANDLE h, const char *name) {
    registry_load();
    char path[192];
    if (!full_path(h, NULL, path, sizeof path)) return ERROR_INVALID_HANDLE;
    regvalue *v = find_value(path, name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    v->used = false;
    registry_dirty = true;
    registry_save();
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegDeleteKeyA(HANDLE parent, const char *sub) {
    registry_load();
    char path[192];
    if (!full_path(parent, sub, path, sizeof path)) return ERROR_INVALID_HANDLE;
    size_t n = strlen(path);
    bool found = false;
    for (int i = 0; i < MAX_VALUES; i++) {
        if (!values[i].used) continue;
        if (strncasecmp(values[i].path, path, n)) continue;
        if (values[i].path[n] && values[i].path[n] != '\\') continue;
        values[i].used = false;
        found = true;
    }
    if (!found) return ERROR_FILE_NOT_FOUND;
    registry_dirty = true;
    registry_save();
    return ERROR_SUCCESS;
}

/* Walking the values under a key, which is how a program lists what it wrote
 * last time without knowing the names in advance. */
static LONG WINAPI w_RegEnumValueA(HANDLE h, DWORD index, char *name, DWORD *name_len,
                                   DWORD *reserved, DWORD *type, BYTE *data, DWORD *len) {
    (void)reserved;
    registry_load();
    char path[192];
    if (!full_path(h, NULL, path, sizeof path)) return ERROR_INVALID_HANDLE;
    DWORD seen = 0;
    for (int i = 0; i < MAX_VALUES; i++) {
        if (!values[i].used || strcasecmp(values[i].path, path)) continue;
        if (seen++ != index) continue;
        if (name && name_len) {
            size_t n = strlcpy(name, values[i].name, *name_len);
            *name_len = (DWORD)n;
        }
        if (type) *type = values[i].type;
        if (data && len) {
            if (*len < values[i].length) { *len = values[i].length; return 234; }
            memcpy(data, values[i].data, values[i].length);
            *len = values[i].length;
        } else if (len) {
            *len = values[i].length;
        }
        return ERROR_SUCCESS;
    }
    return ERROR_NO_MORE_FILES;                    /* also means "no more items" */
}

/* The immediate children of a key, found by looking at what comes after the
 * key's own path in every value below it. */
static LONG WINAPI w_RegEnumKeyExA(HANDLE h, DWORD index, char *name, DWORD *name_len,
                                   DWORD *reserved, char *cls, DWORD *cls_len, FILETIME *ft) {
    (void)reserved; (void)cls; (void)cls_len; (void)ft;
    registry_load();
    char path[192];
    if (!full_path(h, NULL, path, sizeof path)) return ERROR_INVALID_HANDLE;
    size_t plen = strlen(path);

    char seen[16][64];
    int count = 0;
    for (int i = 0; i < MAX_VALUES && count < 16; i++) {
        if (!values[i].used) continue;
        if (strncasecmp(values[i].path, path, plen) || values[i].path[plen] != '\\') continue;
        const char *rest = values[i].path + plen + 1;
        const char *slash = strchr(rest, '\\');
        size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
        if (n >= sizeof seen[0]) n = sizeof seen[0] - 1;

        bool already = false;
        for (int k = 0; k < count; k++)
            if (!strncasecmp(seen[k], rest, n) && strlen(seen[k]) == n) already = true;
        if (already) continue;

        memcpy(seen[count], rest, n);
        seen[count][n] = 0;
        count++;
    }
    if (index >= (DWORD)count) return ERROR_NO_MORE_FILES;
    if (name && name_len) *name_len = (DWORD)strlcpy(name, seen[index], *name_len);
    return ERROR_SUCCESS;
}

static LONG WINAPI w_RegQueryValueA(HANDLE h, const char *sub, char *out, LONG *len) {
    HANDLE key = h;
    LONG r = ERROR_SUCCESS;
    if (sub && *sub) {
        r = w_RegOpenKeyExA(h, sub, 0, 0, &key);
        if (r != ERROR_SUCCESS) return r;
    }
    DWORD dlen = len ? (DWORD)*len : 0;
    r = w_RegQueryValueExA(key, NULL, NULL, NULL, (BYTE *)out, &dlen);
    if (len) *len = (LONG)dlen;
    if (key != h) w_RegCloseKey(key);
    return r;
}

static LONG WINAPI w_RegFlushKey(HANDLE h) { (void)h; registry_save(); return ERROR_SUCCESS; }

/* ------------------------------------------------------------- the rest */

static BOOL WINAPI w_GetUserNameA(char *out, DWORD *cap) {
    const char *name = "user";
    if (!out || !cap || *cap < 5) { if (cap) *cap = 5; return WIN_FALSE; }
    strlcpy(out, name, *cap);
    *cap = (DWORD)strlen(name) + 1;
    return WIN_TRUE;
}

/* Nothing here runs as anybody else, so a program asking whether it is an
 * administrator is told yes: it is the only user there is. */
static BOOL WINAPI w_IsUserAnAdmin(void) { return WIN_TRUE; }

static BOOL WINAPI w_OpenProcessToken(HANDLE process, DWORD access, HANDLE *out) {
    (void)process; (void)access;
    if (out) *out = (HANDLE)(uintptr_t)0x7000;
    return WIN_TRUE;
}
static BOOL WINAPI w_GetTokenInformation(HANDLE token, int cls, void *info, DWORD len, DWORD *got) {
    (void)token; (void)cls; (void)info; (void)len;
    if (got) *got = 0;
    return WIN_FALSE;
}

static const win_export_t advapi32[] = {
    { "RegOpenKeyExA",     (void *)w_RegOpenKeyExA },
    { "RegCreateKeyExA",   (void *)w_RegCreateKeyExA },
    { "RegCloseKey",       (void *)w_RegCloseKey },
    { "RegQueryValueExA",  (void *)w_RegQueryValueExA },
    { "RegQueryValueA",    (void *)w_RegQueryValueA },
    { "RegSetValueExA",    (void *)w_RegSetValueExA },
    { "RegDeleteValueA",   (void *)w_RegDeleteValueA },
    { "RegDeleteKeyA",     (void *)w_RegDeleteKeyA },
    { "RegEnumValueA",     (void *)w_RegEnumValueA },
    { "RegEnumKeyExA",     (void *)w_RegEnumKeyExA },
    { "RegFlushKey",       (void *)w_RegFlushKey },
    { "GetUserNameA",      (void *)w_GetUserNameA },
    { "OpenProcessToken",  (void *)w_OpenProcessToken },
    { "GetTokenInformation", (void *)w_GetTokenInformation },
    { "IsUserAnAdmin",     (void *)w_IsUserAnAdmin },
    { NULL, NULL }
};

void advapi_init(void) { win_register("advapi32.dll", advapi32); }
