// ADVAPI32 registry on a flat file: <game dir>/registry.txt, one value per line:
//   HKLM\SOFTWARE\Activision\Call of Duty\codkey<TAB>type<TAB>hex bytes
// Keys always "exist" (RegOpenKey succeeds); missing values return ERROR_FILE_NOT_FOUND like a fresh install.
// COD_CDKEY=XXXX... seeds HKLM\SOFTWARE\Activision\Call of Duty\codkey (the MP CD key) without editing the file.
#include "../rt.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_MORE_DATA 234
#define REG_SZ 1

typedef struct { char *name; uint32_t type, len; uint8_t *data; } RegVal;
static RegVal *g_vals;
static int g_nvals, g_loaded;
static char *g_keys[256];          // open handle -> key path (handle = 0x1000 + index)

static const char *root_name(uint32_t h) {
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    }
    return NULL;
}
static const char *key_path(uint32_t h) {
    const char *r = root_name(h);
    if (r) return r;
    if (h >= 0x1000 && h < 0x1000 + 256) return g_keys[h - 0x1000];
    return NULL;
}
static void reg_path(char *out, size_t n) {
    snprintf(out, n, "%s/registry.txt", rt_game_dir());
}
static RegVal *find(const char *full) {
    for (int i = 0; i < g_nvals; i++) if (!strcasecmp(g_vals[i].name, full)) return &g_vals[i];
    return NULL;
}
static void set(const char *full, uint32_t type, const uint8_t *data, uint32_t len) {
    RegVal *v = find(full);
    if (!v) {
        g_vals = realloc(g_vals, sizeof *g_vals * (g_nvals + 1));
        v = &g_vals[g_nvals++];
        v->name = strdup(full);
        v->data = NULL;
    }
    free(v->data);
    v->type = type; v->len = len;
    v->data = malloc(len ? len : 1);
    memcpy(v->data, data, len);
}
static void save(void) {
    char p[1024];
    reg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < g_nvals; i++) {
        fprintf(f, "%s\t%u\t", g_vals[i].name, g_vals[i].type);
        for (uint32_t k = 0; k < g_vals[i].len; k++) fprintf(f, "%02x", g_vals[i].data[k]);
        fputc('\n', f);
    }
    fclose(f);
}
static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char p[1024], line[4096];
    reg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *t1 = strchr(line, '\t');
            if (!t1) continue;
            *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t');
            if (!t2) continue;
            uint32_t type = (uint32_t)strtoul(t1 + 1, NULL, 10);
            uint8_t buf[2048];
            uint32_t n = 0;
            for (char *h = t2 + 1; isxdigit((unsigned char)h[0]) && isxdigit((unsigned char)h[1]) && n < sizeof buf; h += 2) {
                char b[3] = {h[0], h[1], 0};
                buf[n++] = (uint8_t)strtoul(b, NULL, 16);
            }
            set(line, type, buf, n);
        }
        fclose(f);
    }
    const char *k = getenv("COD_CDKEY");
    if (k && *k) {
        char clean[64];
        int n = 0;
        for (; *k && n < 63; k++) if (isalnum((unsigned char)*k)) clean[n++] = (char)toupper((unsigned char)*k);
        clean[n] = 0;
        set("HKLM\\SOFTWARE\\Activision\\Call of Duty\\codkey", REG_SZ, (uint8_t *)clean, (uint32_t)n + 1);
    }
}
static void full_name(char *out, size_t n, const char *key, const char *val) {
    snprintf(out, n, "%s\\%s", key, val ? val : "");
}

static uint32_t open_key(uint32_t parent, uint32_t sub, uint32_t out) {
    load();
    const char *base = key_path(parent);
    if (!base) return ERROR_INVALID_HANDLE;
    char path[1024];
    if (sub && *(char *)gptr(sub)) snprintf(path, sizeof path, "%s\\%s", base, (char *)gptr(sub));
    else snprintf(path, sizeof path, "%s", base);
    for (int i = 0; i < 256; i++) {
        if (!g_keys[i]) {
            g_keys[i] = strdup(path);
            g_wr32(out, 0x1000u + (uint32_t)i);
            TRACE("RegOpenKey(%s) -> %x", path, 0x1000 + i);
            return ERROR_SUCCESS;
        }
    }
    return ERROR_FILE_NOT_FOUND;
}
SHIM(ADVAPI32, RegOpenKeyA) { RET(open_key(ARG(0), ARG(1), ARG(2)), 3); }      // (hkey, subkey, phk)
SHIM(ADVAPI32, RegCreateKeyA) { RET(open_key(ARG(0), ARG(1), ARG(2)), 3); }
SHIM(ADVAPI32, RegCloseKey) {
    uint32_t h = ARG(0);
    if (h >= 0x1000 && h < 0x1000 + 256 && g_keys[h - 0x1000]) { free(g_keys[h - 0x1000]); g_keys[h - 0x1000] = NULL; }
    RET(ERROR_SUCCESS, 1);
}
SHIM(ADVAPI32, RegQueryValueExA) {   // (hkey, name, reserved, *type, data, *cbData)
    load();
    const char *k = key_path(ARG(0));
    if (!k) RET(ERROR_INVALID_HANDLE, 6);
    char full[1200];
    full_name(full, sizeof full, k, ARG(1) ? ARGP(1) : NULL);
    RegVal *v = find(full);
    TRACE("RegQueryValueExA(%s) -> %s", full, v ? "found" : "missing");
    if (!v) RET(ERROR_FILE_NOT_FOUND, 6);
    if (ARG(3)) g_wr32(ARG(3), v->type);
    uint32_t cap = ARG(5) ? g_rd32(ARG(5)) : 0;
    if (ARG(5)) g_wr32(ARG(5), v->len);
    if (ARG(4)) {
        if (cap < v->len) RET(ERROR_MORE_DATA, 6);
        memcpy(gptr(ARG(4)), v->data, v->len);
    }
    RET(ERROR_SUCCESS, 6);
}
SHIM(ADVAPI32, RegSetValueExA) {     // (hkey, name, reserved, type, data, cb)
    load();
    const char *k = key_path(ARG(0));
    if (!k) RET(ERROR_INVALID_HANDLE, 6);
    char full[1200];
    full_name(full, sizeof full, k, ARG(1) ? ARGP(1) : NULL);
    set(full, ARG(3), ARG(4) ? gptr(ARG(4)) : (const uint8_t *)"", ARG(4) ? ARG(5) : 0);
    save();
    RET(ERROR_SUCCESS, 6);
}
