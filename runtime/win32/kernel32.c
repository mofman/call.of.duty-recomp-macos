// KERNEL32 shims.
#include "../rt.h"
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>

void rt_set_cwd(const char *wp);
const char *rt_get_cwd(void);
const char *rt_exe_path(void);

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_NO_MORE_FILES 18
#define ERROR_ALREADY_EXISTS 183
#define INVALID_HANDLE 0xFFFFFFFFu

static uint32_t g_cmdline, g_env_a, g_env_w;

// ------------------------------------------------------------------ process / startup
const char *rt_cmdline(void);
SHIM(KERNEL32, GetCommandLineA) {
    if (!g_cmdline) g_cmdline = gstrdup(rt_cmdline());
    RET(g_cmdline, 0);
}
SHIM(KERNEL32, GetVersion) { RET(0x0A280105, 0); }   // Windows XP 5.1 build 2600
SHIM(KERNEL32, GetVersionExA) {
    uint32_t p = ARG(0);
    uint32_t sz = g_rd32(p);
    memset(gptr(p + 4), 0, sz - 4);
    g_wr32(p + 4, 5); g_wr32(p + 8, 1); g_wr32(p + 12, 2600); g_wr32(p + 16, 2);   // VER_PLATFORM_WIN32_NT
    strcpy((char *)gptr(p + 20), "Service Pack 3");
    RET(1, 1);
}
SHIM(KERNEL32, GetStartupInfoA) {
    uint32_t p = ARG(0);
    memset(gptr(p), 0, 68);
    g_wr32(p, 68);
    RET(0, 1);
}
#define GL_HMODULE 0x7F000000u
uint32_t gl_proc_address(const char *name);    // gl/ shim: thunk for a gl*/wgl* entry point (0 if unknown)
static const char *base_name(const char *p) {
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}
SHIM(KERNEL32, GetModuleHandleA) {
    uint32_t n = ARG(0);
    TRACE("GetModuleHandleA(%s)", n ? ARGP(0) : "NULL");
    if (!n) RET(g_hinstance, 1);
    const char *b = base_name(ARGP(0));
    if (!strncasecmp(b, "opengl32", 8)) RET(GL_HMODULE, 1);
    if (!strcasecmp(b, RT_EXE_NAME)) RET(g_hinstance, 1);
    RET(0x77000000u | (uint32_t)(strlen(b) << 8), 1);   // system DLLs: opaque non-null handles
}
SHIM(KERNEL32, GetModuleFileNameA) {
    const char *mn = rt_module_name_of(ARG(0) ? ARG(0) : g_hinstance);
    char s[512];
    snprintf(s, sizeof s, "C:\\" RT_VIRTUAL_INSTALL_DIR "\\%s", mn ? mn : RT_EXE_NAME);
    uint32_t buf = ARG(1), n = ARG(2);
    strncpy((char *)gptr(buf), s, n);
    if (n) gptr(buf)[n - 1] = 0;
    RET(strlen(s) < n ? strlen(s) : n, 3);
}
SHIM(KERNEL32, LoadLibraryA) {
    const char *name = ARGP(0), *b = base_name(name);
    uint32_t h = 0;
    if (!strncasecmp(b, "opengl32", 8)) h = GL_HMODULE;
    else h = rt_load_module(cpu, name);
    if (h) LOG("LoadLibraryA(%s) -> %08x", name, h);
    else {   // log each missing DLL once (CoDMP retries PunkBuster's pbcl/pbsv.dll every frame)
        static char missing[16][64];
        int i, known = 0;
        for (i = 0; i < 16 && missing[i][0]; i++) if (!strcasecmp(missing[i], b)) { known = 1; break; }
        if (!known) {
            LOG("LoadLibraryA(%s) -> not found%s", name, i < 16 ? "" : " (further misses not logged)");
            if (i < 16) snprintf(missing[i], sizeof missing[i], "%s", b);
        }
        rt_set_last_error(126);   // ERROR_MOD_NOT_FOUND
    }
    RET(h, 1);
}
SHIM(KERNEL32, FreeLibrary) {
    uint32_t h = ARG(0);
    if (h != GL_HMODULE) rt_free_module(cpu, h);
    RET(1, 1);
}
SHIM(KERNEL32, GetProcAddress) {
    uint32_t h = ARG(0), n = ARG(1);
    const char *name = n < 0x10000 ? "#ord" : ARGP(1);
    uint32_t t = 0;
    if (n >= 0x10000) {
        if (h == GL_HMODULE) t = gl_proc_address(name);
        else if (rt_module_name_of(h) && h != g_hinstance) t = rt_module_export(h, name);
        else t = rt_import_thunk("", name);
    }
    TRACE("GetProcAddress(%08x, %s) -> %08x", h, name, t);
    if (!t && h != GL_HMODULE) LOG("GetProcAddress: %s not available", name);
    if (!t) rt_set_last_error(127);
    RET(t, 2);
}
SHIM(KERNEL32, ExitProcess) { LOG("ExitProcess(%u) from %08x", ARG(0), g_rd32(cpu->esp)); fflush(stderr); _exit((int)ARG(0)); }
SHIM(KERNEL32, TerminateProcess) { LOG("TerminateProcess(%u)", ARG(1)); exit((int)ARG(1)); }
SHIM(KERNEL32, FatalAppExitA) { LOG("FatalAppExit: %s", ARGP(1)); exit(1); }
SHIM(KERNEL32, GetCurrentProcess) { RET(0xFFFFFFFFu, 0); }
SHIM(KERNEL32, GetCurrentThread) { RET(0xFFFFFFFEu, 0); }
SHIM(KERNEL32, GetCurrentThreadId) { RET(g_rd32(cpu->fs_base + 0x24), 0); }
SHIM(KERNEL32, SetUnhandledExceptionFilter) { RET(0, 1); }
SHIM(KERNEL32, UnhandledExceptionFilter) { RET(1, 1); }
SHIM(KERNEL32, SetConsoleCtrlHandler) { RET(1, 2); }
SHIM(KERNEL32, RaiseException) {
    LOG("RaiseException code=%08x flags=%x nargs=%u", ARG(0), ARG(1), ARG(2));
    trap(cpu, g_rd32(cpu->esp), "RaiseException (C++ exception / SEH not supported yet)");
}
SHIM(KERNEL32, RtlUnwind) { RET(0, 4); }
SHIM(KERNEL32, OutputDebugStringA) { LOG("ODS: %s", ARGP(0)); RET(0, 1); }
SHIM(KERNEL32, GetLastError) { RET(rt_last_error(), 0); }
SHIM(KERNEL32, SetLastError) { rt_set_last_error(ARG(0)); RET(0, 1); }
SHIM(KERNEL32, FormatMessageA) {
    uint32_t buf = ARG(4);
    if (ARG(0) & 0x100) {   // ALLOCATE_BUFFER
        uint32_t p = gstrdup("Error.\r\n");
        g_wr32(buf, p);
    } else if (ARG(5)) strncpy((char *)gptr(buf), "Error.\r\n", ARG(5));
    RET(8, 7);
}
SHIM(KERNEL32, CreateProcessA) { LOG("CreateProcessA(%s) refused", ARG(1) ? ARGP(1) : ""); RET(0, 10); }
SHIM(KERNEL32, GetExitCodeProcess) { g_wr32(ARG(1), 0); RET(1, 2); }

// ------------------------------------------------------------------ environment
SHIM(KERNEL32, GetEnvironmentStrings) {
    if (!g_env_a) { g_env_a = galloc(64); memcpy(gptr(g_env_a), "PATH=C:\\\0\0", 10); }
    RET(g_env_a, 0);
}
SHIM(KERNEL32, GetEnvironmentStringsW) {
    if (!g_env_w) {
        g_env_w = galloc(64);
        const char *s = "PATH=C:\\";
        for (int i = 0; s[i]; i++) g_wr16(g_env_w + 2 * i, s[i]);
    }
    RET(g_env_w, 0);
}
SHIM(KERNEL32, FreeEnvironmentStringsA) { RET(1, 1); }
SHIM(KERNEL32, FreeEnvironmentStringsW) { RET(1, 1); }
SHIM(KERNEL32, SetEnvironmentVariableA) { RET(1, 2); }
SHIM(KERNEL32, SetHandleCount) { RET(ARG(0), 1); }
SHIM(KERNEL32, GetStdHandle) { RET(0x10 + (ARG(0) & 0xf), 1); }   // fake console handles
SHIM(KERNEL32, GetFileType) {
    uint32_t h = ARG(0);
    if (h >= 0x10 && h < 0x20) RET(2, 1);   // FILE_TYPE_CHAR
    RET(h_get(h) ? 1 : 0, 1);
}
SHIM(KERNEL32, SetStdHandle) { RET(1, 2); }

// ------------------------------------------------------------------ heap / memory
SHIM(KERNEL32, HeapCreate) { RET(0x00500000, 3); }
SHIM(KERNEL32, HeapDestroy) { RET(1, 1); }
void rt_host_bt(void);
SHIM(KERNEL32, HeapAlloc) {
    uint32_t p = galloc(ARG(2) ? ARG(2) : 1);
    if (getenv("COD_ALLOCLOG") && ARG(2) >= 512 * 1024) { fprintf(stderr, "[alloc] %u bytes -> %08x\n", ARG(2), p); rt_host_bt(); }
    RET(p, 3);
}
SHIM(KERNEL32, HeapFree) { gfree(ARG(2)); RET(1, 3); }
SHIM(KERNEL32, HeapReAlloc) {
    if (ARG(1) & 0x10) {   // HEAP_REALLOC_IN_PLACE_ONLY
        uint32_t p = ARG(2);
        if (ARG(3) <= g_rd32(p - 16)) { grealloc(p, ARG(3)); RET(p, 4); }
        RET(0, 4);
    }
    RET(grealloc(ARG(2), ARG(3)), 4);
}
SHIM(KERNEL32, HeapSize) { RET(gsize(ARG(2)), 3); }
SHIM(KERNEL32, VirtualAlloc) {
    uint32_t addr = ARG(0), size = ARG(1);
    if (addr) RET(addr & ~0xfffu, 4);       // commit within an earlier reservation
    uint32_t p = gvalloc(size);      // fresh address space is already zero (never reused)
    if (!p) LOG("VirtualAlloc(%u) failed: guest address space exhausted", size);
    RET(p, 4);
}
SHIM(KERNEL32, VirtualFree) { RET(1, 3); }
SHIM(KERNEL32, GlobalAlloc) { uint32_t p = galloc(ARG(1) ? ARG(1) : 1); RET(p, 2); }
SHIM(KERNEL32, GlobalLock) { RET(ARG(0), 1); }
SHIM(KERNEL32, GlobalUnlock) { RET(1, 1); }
SHIM(KERNEL32, LocalFree) { gfree(ARG(0)); RET(0, 1); }
SHIM(KERNEL32, IsBadReadPtr) { RET(ARG(0) < 0x10000, 2); }
SHIM(KERNEL32, IsBadWritePtr) { RET(ARG(0) < 0x10000, 2); }
SHIM(KERNEL32, IsBadCodePtr) { RET(ARG(0) < 0x10000, 1); }

// ------------------------------------------------------------------ sync
typedef struct { pthread_mutex_t m; } CS;
static CS *cs_get(uint32_t p) {
    // guest CRITICAL_SECTION (24 bytes): we keep a host mutex pointer in DebugInfo slot
    uint32_t idx = g_rd32(p);
    if (idx == 0) return NULL;
    return (CS *)(uintptr_t)(((uint64_t)g_rd32(p + 4) << 32) | g_rd32(p + 8));
}
SHIM(KERNEL32, InitializeCriticalSection) {
    uint32_t p = ARG(0);
    CS *c = malloc(sizeof *c);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->m, &a);
    uint64_t v = (uint64_t)(uintptr_t)c;
    g_wr32(p, 1); g_wr32(p + 4, (uint32_t)(v >> 32)); g_wr32(p + 8, (uint32_t)v);
    RET(0, 1);
}
SHIM(KERNEL32, DeleteCriticalSection) { RET(0, 1); }
static void checkpoint(void);
SHIM(KERNEL32, EnterCriticalSection) {
    checkpoint();
    CS *c = cs_get(ARG(0));
    if (c) pthread_mutex_lock(&c->m);
    RET(0, 1);
}
SHIM(KERNEL32, LeaveCriticalSection) {
    CS *c = cs_get(ARG(0));
    if (c) pthread_mutex_unlock(&c->m);
    RET(0, 1);
}
SHIM(KERNEL32, InterlockedIncrement) { RET(__atomic_add_fetch((uint32_t *)gptr(ARG(0)), 1, __ATOMIC_SEQ_CST), 1); }
SHIM(KERNEL32, InterlockedDecrement) { RET(__atomic_sub_fetch((uint32_t *)gptr(ARG(0)), 1, __ATOMIC_SEQ_CST), 1); }

typedef struct { uint32_t start, param; Cpu cpu; uint32_t handle; pthread_t th;
                 pthread_mutex_t m; pthread_cond_t c; int suspend, parked; } ThreadStart;
static __thread ThreadStart *t_self;
// cooperative suspension point: guest threads park here while SuspendThread is in effect
static void checkpoint(void) {
    ThreadStart *ts = t_self;
    if (!ts || !ts->suspend) return;
    pthread_mutex_lock(&ts->m);
    while (ts->suspend > 0) { ts->parked = 1; pthread_cond_broadcast(&ts->c); pthread_cond_wait(&ts->c, &ts->m); }
    ts->parked = 0;
    pthread_mutex_unlock(&ts->m);
}
typedef struct { pthread_mutex_t m; pthread_cond_t c; int signaled, manual; } Event;
SHIM(KERNEL32, CreateEventA) {
    Event *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL); pthread_cond_init(&e->c, NULL);
    e->manual = ARG(1); e->signaled = ARG(2);
    uint32_t h = h_new(H_EVENT, e);
    TRACE("CreateEventA manual=%u init=%u -> %x (caller %08x)", ARG(1), ARG(2), h, g_rd32(cpu->esp));
    RET(h, 4);
}
SHIM(KERNEL32, CreateMutexA) {
    rt_set_last_error(0);
    RET(h_new(H_MUTEX, NULL), 3);
}
SHIM(KERNEL32, ReleaseMutex) { RET(1, 1); }
void rt_event_set(uint32_t h) {
    RtHandle *x = h_get(h);
    if (!x || x->kind != H_EVENT) return;
    Event *e = x->p;
    pthread_mutex_lock(&e->m); e->signaled = 1; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m);
}
uint32_t rt_wait(uint32_t h, uint32_t ms) {
    RtHandle *x = h_get(h);
    if (!x) return 0;
    if (x->kind == H_EVENT) {
        Event *e = x->p;
        pthread_mutex_lock(&e->m);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000; ts.tv_nsec += (ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int rc = 0;
        while (!e->signaled && rc == 0) rc = ms == 0xFFFFFFFFu ? pthread_cond_wait(&e->c, &e->m) : pthread_cond_timedwait(&e->c, &e->m, &ts);
        int ok = e->signaled;
        if (ok && !e->manual) e->signaled = 0;
        pthread_mutex_unlock(&e->m);
        return ok ? 0 : 0x102;   // WAIT_TIMEOUT
    }
    if (x->kind == H_THREAD) {
        if (ms == 0xFFFFFFFFu) { pthread_join(((ThreadStart *)x->p)->th, NULL); return 0; }
        return x->extra ? 0 : 0x102;
    }
    return 0;
}
// non-blocking check: returns 1 (and consumes auto-reset events) if handle is signaled
int rt_handle_try(uint32_t h) {
    RtHandle *x = h_get(h);
    if (!x) return 1;
    if (x->kind == H_EVENT) {
        Event *e = x->p;
        pthread_mutex_lock(&e->m);
        int ok = e->signaled;
        if (ok && !e->manual) e->signaled = 0;
        pthread_mutex_unlock(&e->m);
        return ok;
    }
    if (x->kind == H_THREAD) return x->extra != 0;
    return 1;
}
void rt_checkpoint(void) { checkpoint(); }
SHIM(KERNEL32, WaitForSingleObject) { if (h_get(ARG(0)) && h_get(ARG(0))->kind == H_EVENT) TRACE("WaitForSingleObject(%x, %u) caller %08x", ARG(0), ARG(1), g_rd32(cpu->esp)); checkpoint(); uint32_t r = rt_wait(ARG(0), ARG(1)); checkpoint(); RET(r, 2); }

// ------------------------------------------------------------------ threads
static void *thread_main(void *p) {
    ThreadStart *ts = p;
    pthread_mutex_lock(&ts->m);
    while (ts->suspend > 0) pthread_cond_wait(&ts->c, &ts->m);
    pthread_mutex_unlock(&ts->m);
    t_self = ts;
    rt_new_thread_cpu(&ts->cpu);
    uint32_t r = call_guest(&ts->cpu, ts->start, 0, 1, ts->param);
    RtHandle *h = h_get(ts->handle);
    if (h) h->extra = 1;
    (void)r;
    return NULL;
}
SHIM(KERNEL32, CreateThread) {
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->start = ARG(2); ts->param = ARG(3);
    pthread_mutex_init(&ts->m, NULL); pthread_cond_init(&ts->c, NULL);
    ts->suspend = (ARG(4) & 4) ? 1 : 0;   // CREATE_SUSPENDED
    pthread_t *t = &ts->th;
    uint32_t h = h_new(H_THREAD, ts);
    ts->handle = h;
    LOG("CreateThread start=%08x param=%08x flags=%x", ts->start, ts->param, ARG(4));
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    pthread_create(t, &a, thread_main, ts);
    if (ARG(5)) g_wr32(ARG(5), 0x200 + h);
    RET(h, 6);
}
SHIM(KERNEL32, TerminateThread) { RET(1, 2); }
SHIM(KERNEL32, SuspendThread) {
    RtHandle *h = h_get(ARG(0));
    if (!h || h->kind != H_THREAD) RET(0xFFFFFFFFu, 1);
    ThreadStart *ts = h->p;
    pthread_mutex_lock(&ts->m);
    int prev = ts->suspend++;
    // wait (bounded) until the thread parks at a checkpoint, so the caller can touch its data safely
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 200000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    while (ts != t_self && !ts->parked && !h->extra && ts->suspend > 0)
        if (pthread_cond_timedwait(&ts->c, &ts->m, &dl)) { LOG("SuspendThread: thread did not park within 200ms"); break; }
    pthread_mutex_unlock(&ts->m);
    RET(prev, 1);
}
SHIM(KERNEL32, ResumeThread) {
    RtHandle *h = h_get(ARG(0));
    if (!h || h->kind != H_THREAD) RET(0xFFFFFFFFu, 1);
    ThreadStart *ts = h->p;
    pthread_mutex_lock(&ts->m);
    int prev = ts->suspend;
    if (ts->suspend > 0) ts->suspend--;
    pthread_cond_broadcast(&ts->c);
    pthread_mutex_unlock(&ts->m);
    RET(prev, 1);
}
SHIM(KERNEL32, SetThreadPriority) { RET(1, 2); }
SHIM(KERNEL32, CloseHandle);
SHIM(KERNEL32, Sleep) { checkpoint(); usleep(ARG(0) * 1000); checkpoint(); RET(0, 1); }
SHIM(KERNEL32, SleepEx) { checkpoint(); usleep(ARG(0) * 1000); checkpoint(); RET(0, 2); }

static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;
static uint32_t g_tls_next;
static void tls_init(void) { pthread_key_create(&g_tls_key, NULL); }
static uint32_t *tls_slots(void) {
    pthread_once(&g_tls_once, tls_init);
    uint32_t *s = pthread_getspecific(g_tls_key);
    if (!s) { s = calloc(64, 4); pthread_setspecific(g_tls_key, s); }
    return s;
}
SHIM(KERNEL32, TlsAlloc) { tls_slots(); RET(__atomic_fetch_add(&g_tls_next, 1, __ATOMIC_SEQ_CST), 0); }
SHIM(KERNEL32, TlsFree) { RET(1, 1); }
SHIM(KERNEL32, TlsGetValue) { rt_set_last_error(0); RET(tls_slots()[ARG(0) & 63], 1); }
SHIM(KERNEL32, TlsSetValue) { tls_slots()[ARG(0) & 63] = ARG(1); RET(1, 2); }

// ------------------------------------------------------------------ time
SHIM(KERNEL32, GetTickCount) {
    static uint64_t t0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!t0) t0 = ms - 100000;
    RET((uint32_t)(ms - t0), 0);
}
SHIM(KERNEL32, QueryPerformanceFrequency) { g_wr32(ARG(0), 1000000); g_wr32(ARG(0) + 4, 0); RET(1, 1); }
SHIM(KERNEL32, QueryPerformanceCounter) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    g_wr32(ARG(0), (uint32_t)us); g_wr32(ARG(0) + 4, (uint32_t)(us >> 32));
    RET(1, 1);
}
static void put_systemtime(uint32_t p, struct tm *t, int ms) {
    g_wr16(p, t->tm_year + 1900); g_wr16(p + 2, t->tm_mon + 1); g_wr16(p + 4, t->tm_wday); g_wr16(p + 6, t->tm_mday);
    g_wr16(p + 8, t->tm_hour); g_wr16(p + 10, t->tm_min); g_wr16(p + 12, t->tm_sec); g_wr16(p + 14, ms);
}
SHIM(KERNEL32, GetLocalTime) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm t; localtime_r(&tv.tv_sec, &t);
    put_systemtime(ARG(0), &t, tv.tv_usec / 1000);
    RET(0, 1);
}
SHIM(KERNEL32, GetTimeZoneInformation) { memset(gptr(ARG(0)), 0, 172); RET(0, 1); }
static uint64_t ft_read(uint32_t p) { return ((uint64_t)g_rd32(p + 4) << 32) | g_rd32(p); }
SHIM(KERNEL32, FileTimeToLocalFileTime) { g_wr32(ARG(1), g_rd32(ARG(0))); g_wr32(ARG(1) + 4, g_rd32(ARG(0) + 4)); RET(1, 2); }
SHIM(KERNEL32, FileTimeToSystemTime) {
    uint64_t ft = ft_read(ARG(0));
    time_t s = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm t; gmtime_r(&s, &t);
    put_systemtime(ARG(1), &t, (int)((ft / 10000) % 1000));
    RET(1, 2);
}

// ------------------------------------------------------------------ locale / strings (single-byte, cp1252-ish)
SHIM(KERNEL32, GetACP) { RET(1252, 0); }
SHIM(KERNEL32, GetOEMCP) { RET(437, 0); }
SHIM(KERNEL32, GetCPInfo) {
    uint32_t p = ARG(1);
    memset(gptr(p), 0, 20);
    g_wr32(p, 1); gptr(p + 4)[0] = '?';
    RET(1, 2);
}
SHIM(KERNEL32, IsValidCodePage) { RET(1, 1); }
SHIM(KERNEL32, IsValidLocale) { RET(1, 2); }
SHIM(KERNEL32, GetUserDefaultLCID) { RET(0x409, 0); }
SHIM(KERNEL32, EnumSystemLocalesA) { RET(1, 2); }
SHIM(KERNEL32, GetLocaleInfoA) {
    uint32_t type = ARG(1) & 0xffff, buf = ARG(2), n = ARG(3);
    const char *v = "";
    char tmp[16];
    switch (type) {
    case 0x1004: v = "1252"; break;     // LOCALE_IDEFAULTANSICODEPAGE
    case 0x000B: v = "437"; break;      // OEM codepage
    case 0x0003: v = "ENU"; break;
    case 0x0007: v = "USA"; break;
    case 0x0001: v = "0409"; break;
    case 0x000E: v = "."; break;
    case 0x000F: v = ","; break;
    default: snprintf(tmp, sizeof tmp, "0"); v = tmp;
    }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (n == 0) RET(len, 4);
    strncpy((char *)gptr(buf), v, n);
    RET(len, 4);
}
SHIM(KERNEL32, GetLocaleInfoW) {
    uint32_t buf = ARG(2), n = ARG(3);
    if (n == 0) RET(2, 4);
    g_wr16(buf, '0'); if (n > 1) g_wr16(buf + 2, 0);
    RET(2, 4);
}
SHIM(KERNEL32, MultiByteToWideChar) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) n = (int32_t)strlen((char *)gptr(src)) + 1;
    if (!cap) RET(n, 6);
    int32_t k = 0;
    for (; k < n && (uint32_t)k < cap; k++) g_wr16(dst + 2 * k, gptr(src)[k]);
    RET(k, 6);
}
SHIM(KERNEL32, WideCharToMultiByte) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) { n = 0; while (g_rd16(src + 2 * n)) n++; n++; }
    if (!cap) RET(n, 8);
    int32_t k = 0;
    for (; k < n && (uint32_t)k < cap; k++) { uint16_t c = g_rd16(src + 2 * k); gptr(dst)[k] = c < 256 ? (uint8_t)c : '?'; }
    RET(k, 8);
}
SHIM(KERNEL32, GetStringTypeA) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t out = ARG(4);
    if (n < 0) n = (int32_t)strlen((char *)gptr(src)) + 1;
    for (int i = 0; i < n; i++) {
        uint8_t c = gptr(src)[i]; uint16_t t = 0;
        if (c >= 'A' && c <= 'Z') t |= 0x101; if (c >= 'a' && c <= 'z') t |= 0x102;
        if (c >= '0' && c <= '9') t |= 0x84; if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8 | (c == ' ' ? 0x40 : 0);
        if (c < 32 || c == 127) t |= 0x20; if (c > 32 && c < 127 && !(t & 0x103) && !(t & 4)) t |= 0x10;
        g_wr16(out + 2 * i, t);
    }
    RET(1, 5);
}
SHIM(KERNEL32, GetStringTypeW) {
    uint32_t src = ARG(1); int32_t n = (int32_t)ARG(2); uint32_t out = ARG(3);
    if (n < 0) { n = 0; while (g_rd16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) {
        uint16_t c = g_rd16(src + 2 * i), t = 0;
        if (c >= 'A' && c <= 'Z') t |= 0x101; if (c >= 'a' && c <= 'z') t |= 0x102;
        if (c >= '0' && c <= '9') t |= 0x84; if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8 | (c == ' ' ? 0x40 : 0);
        if (c < 32 || c == 127) t |= 0x20; if (c > 32 && c < 127 && !(t & 0x103) && !(t & 4)) t |= 0x10;
        g_wr16(out + 2 * i, t);
    }
    RET(1, 4);
}
SHIM(KERNEL32, LCMapStringA) {
    uint32_t flags = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) n = (int32_t)strlen((char *)gptr(src)) + 1;
    if (!cap) RET(n, 6);
    for (int i = 0; i < n && (uint32_t)i < cap; i++) {
        uint8_t c = gptr(src)[i];
        if (flags & 0x100) c = (c >= 'a' && c <= 'z') ? c - 32 : c;   // LCMAP_UPPERCASE
        if (flags & 0x200) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;   // LCMAP_LOWERCASE
        gptr(dst)[i] = c;
    }
    RET(n, 6);
}
SHIM(KERNEL32, LCMapStringW) {
    uint32_t flags = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) { n = 0; while (g_rd16(src + 2 * n)) n++; n++; }
    if (!cap) RET(n, 6);
    for (int i = 0; i < n && (uint32_t)i < cap; i++) {
        uint16_t c = g_rd16(src + 2 * i);
        if (flags & 0x100) c = (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (flags & 0x200) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        g_wr16(dst + 2 * i, c);
    }
    RET(n, 6);
}
static int cmp_ci(int ci, int a, int b) {
    if (ci) { if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; }
    return a - b;
}
SHIM(KERNEL32, CompareStringA) {
    int ci = ARG(1) & 1;
    const uint8_t *a = gptr(ARG(2)), *b = gptr(ARG(4));
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = (int32_t)strlen((const char *)a);
    if (nb < 0) nb = (int32_t)strlen((const char *)b);
    int r = 0;
    for (int i = 0; !r && i < na && i < nb; i++) r = cmp_ci(ci, a[i], b[i]);
    if (!r) r = na - nb;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
SHIM(KERNEL32, CompareStringW) {
    int ci = ARG(1) & 1;
    uint32_t a = ARG(2), b = ARG(4);
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) { na = 0; while (g_rd16(a + 2 * na)) na++; }
    if (nb < 0) { nb = 0; while (g_rd16(b + 2 * nb)) nb++; }
    int r = 0;
    for (int i = 0; !r && i < na && i < nb; i++) r = cmp_ci(ci, g_rd16(a + 2 * i), g_rd16(b + 2 * i));
    if (!r) r = na - nb;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}

// ------------------------------------------------------------------ files
typedef struct { int fd; char path[PATH_MAX]; } FileH;

// create missing parent directories of a host path (Windows installs always have the save/ tree)
static void mkparents(const char *path) {
    char t[PATH_MAX];
    snprintf(t, sizeof t, "%s", path);
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
}
SHIM(KERNEL32, CreateFileA) {
    char host[PATH_MAX];
    const char *wp = ARGP(0);
    uint32_t access = ARG(1), disp = ARG(4);
    rt_map_path(wp, host, sizeof host);
    int fl = 0;
    if ((access & 0xC0000000u) == 0xC0000000u) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;    // CREATE_NEW
    case 2: fl |= O_CREAT | O_TRUNC; break;   // CREATE_ALWAYS
    case 3: break;                            // OPEN_EXISTING
    case 4: fl |= O_CREAT; break;             // OPEN_ALWAYS
    case 5: fl |= O_TRUNC; break;             // TRUNCATE_EXISTING
    }
    if (fl & (O_CREAT | O_WRONLY | O_RDWR)) mkparents(host);
    int fd = open(host, fl, 0644);
    TRACE("CreateFileA(%s -> %s, %08x, %u) = %d", wp, host, access, disp, fd);
    if (fd < 0) {
        rt_set_last_error(errno == ENOENT ? ERROR_FILE_NOT_FOUND : 5);
        RET(INVALID_HANDLE, 7);
    }
    FileH *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    uint32_t h = h_new(H_FILE, f);
    h_get(h)->fd = fd;
    rt_set_last_error(0);
    RET(h, 7);
}
SHIM(KERNEL32, ReadFile) {
    RtHandle *h = h_get(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), outp = ARG(3);
    if (!h || h->kind != H_FILE) { if (outp) g_wr32(outp, 0); RET(0, 5); }
    ssize_t r = read(h->fd, gptr(buf), n);
    if (outp) g_wr32(outp, r < 0 ? 0 : (uint32_t)r);
    RET(r >= 0, 5);
}
SHIM(KERNEL32, WriteFile) {
    uint32_t hh = ARG(0), buf = ARG(1), n = ARG(2), outp = ARG(3);
    if (hh >= 0x10 && hh < 0x20) {   // console
        fwrite(gptr(buf), 1, n, stderr);
        if (outp) g_wr32(outp, n);
        RET(1, 5);
    }
    RtHandle *h = h_get(hh);
    if (!h || h->kind != H_FILE) RET(0, 5);
    ssize_t r = write(h->fd, gptr(buf), n);
    if (outp) g_wr32(outp, r < 0 ? 0 : (uint32_t)r);
    RET(r >= 0, 5);
}
SHIM(KERNEL32, SetFilePointer) {
    RtHandle *h = h_get(ARG(0));
    int32_t lo = (int32_t)ARG(1);
    uint32_t hip = ARG(2), how = ARG(3);
    if (!h || h->kind != H_FILE) RET(0xFFFFFFFFu, 4);
    int64_t off = lo;
    if (hip) off = ((int64_t)(int32_t)g_rd32(hip) << 32) | (uint32_t)lo;
    off_t r = lseek(h->fd, off, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END);
    if (hip) g_wr32(hip, (uint32_t)((uint64_t)r >> 32));
    RET((uint32_t)r, 4);
}
SHIM(KERNEL32, SetEndOfFile) {
    RtHandle *h = h_get(ARG(0));
    if (!h) RET(0, 1);
    ftruncate(h->fd, lseek(h->fd, 0, SEEK_CUR));
    RET(1, 1);
}
SHIM(KERNEL32, FlushFileBuffers) { RET(1, 1); }
typedef struct { DIR *d; char dir[PATH_MAX]; char pat[256]; } FindH;
SHIM(KERNEL32, CloseHandle) {
    uint32_t hh = ARG(0);
    RtHandle *h = h_get(hh);
    if (h) {
        if (h->kind == H_FILE) { close(h->fd); free(h->p); }
        if (h->kind == H_EVENT) TRACE("CloseHandle(event %x)", hh);
        h_free(hh);
    }
    RET(1, 1);
}

static void fill_find_data(uint32_t p, const char *dir, const char *name) {
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    struct stat st;
    stat(full, &st);
    memset(gptr(p), 0, 320);
    g_wr32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    for (int k = 0; k < 3; k++) { g_wr32(p + 4 + 8 * k, (uint32_t)ft); g_wr32(p + 8 + 8 * k, (uint32_t)(ft >> 32)); }
    g_wr32(p + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    g_wr32(p + 32, (uint32_t)st.st_size);
    strncpy((char *)gptr(p + 44), name, 259);
}
static int find_next(FindH *f, uint32_t data) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD) == 0) {
            fill_find_data(data, f->dir, e->d_name);
            return 1;
        }
    }
    return 0;
}
SHIM(KERNEL32, FindFirstFileA) {
    char host[PATH_MAX];
    const char *wp = ARGP(0);
    // split pattern
    char wdir[PATH_MAX];
    snprintf(wdir, sizeof wdir, "%s", wp);
    char *slash = strrchr(wdir, '\\');
    char *slash2 = strrchr(wdir, '/');
    if (slash2 > slash) slash = slash2;
    FindH *f = calloc(1, sizeof *f);
    if (slash) { snprintf(f->pat, sizeof f->pat, "%s", slash + 1); *slash = 0; rt_map_path(wdir, host, sizeof host); }
    else { snprintf(f->pat, sizeof f->pat, "%s", wdir); rt_map_path(".", host, sizeof host); }
    if (!strcmp(f->pat, "*.*")) strcpy(f->pat, "*");
    snprintf(f->dir, sizeof f->dir, "%s", host);
    f->d = opendir(host);
    TRACE("FindFirstFileA(%s) dir=%s pat=%s", wp, host, f->pat);
    if (!f->d || !find_next(f, ARG(1))) {
        if (f->d) closedir(f->d);
        free(f);
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    RET(h_new(H_FIND, f), 2);
}
SHIM(KERNEL32, FindNextFileA) {
    RtHandle *h = h_get(ARG(0));
    if (!h || h->kind != H_FIND) RET(0, 2);
    if (!find_next(h->p, ARG(1))) { rt_set_last_error(ERROR_NO_MORE_FILES); RET(0, 2); }
    RET(1, 2);
}
SHIM(KERNEL32, FindClose) {
    RtHandle *h = h_get(ARG(0));
    if (h && h->kind == H_FIND) { FindH *f = h->p; closedir(f->d); free(f); h_free(ARG(0)); }
    RET(1, 1);
}
SHIM(KERNEL32, GetFileAttributesA) {
    char host[PATH_MAX];
    rt_map_path(ARGP(0), host, sizeof host);
    struct stat st;
    if (stat(host, &st) != 0) { rt_set_last_error(ERROR_FILE_NOT_FOUND); RET(0xFFFFFFFFu, 1); }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
}
SHIM(KERNEL32, SetFileAttributesA) { RET(1, 2); }
SHIM(KERNEL32, CreateDirectoryA) {
    char host[PATH_MAX];
    rt_map_path(ARGP(0), host, sizeof host);
    if (mkdir(host, 0755) != 0) { rt_set_last_error(ERROR_ALREADY_EXISTS); RET(0, 2); }
    RET(1, 2);
}
SHIM(KERNEL32, CopyFileA) {
    char a[PATH_MAX], b[PATH_MAX];
    rt_map_path(ARGP(0), a, sizeof a); rt_map_path(ARGP(1), b, sizeof b);
    FILE *fa = fopen(a, "rb");
    if (!fa) RET(0, 3);
    if (ARG(2)) { struct stat st; if (stat(b, &st) == 0) { fclose(fa); RET(0, 3); } }
    mkparents(b);
    FILE *fb = fopen(b, "wb");
    if (!fb) { fclose(fa); RET(0, 3); }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, fa)) > 0) fwrite(buf, 1, n, fb);
    fclose(fa); fclose(fb);
    RET(1, 3);
}
SHIM(KERNEL32, GetCurrentDirectoryA) {
    char s[PATH_MAX];
    const char *c = rt_get_cwd();
    snprintf(s, sizeof s, "C:\\" RT_VIRTUAL_INSTALL_DIR "%s%s", c[0] ? "\\" : "", c);
    for (char *p = s; *p; p++) if (*p == '/') *p = '\\';
    uint32_t n = ARG(0), buf = ARG(1);
    uint32_t len = (uint32_t)strlen(s);
    if (n <= len) RET(len + 1, 2);
    strcpy((char *)gptr(buf), s);
    RET(len, 2);
}
SHIM(KERNEL32, SetCurrentDirectoryA) {
    TRACE("SetCurrentDirectoryA(%s)", ARGP(0));
    const char *p = ARGP(0);
    // "C:\CoD\X" -> relative "X"
    if (!strncasecmp(p, "C:\\" RT_VIRTUAL_INSTALL_DIR, 2 + strlen(RT_VIRTUAL_INSTALL_DIR))) { p += 2 + strlen(RT_VIRTUAL_INSTALL_DIR); if (*p == '\\') p++; char t[PATH_MAX]; snprintf(t, sizeof t, "\\%s", p); rt_set_cwd(t); }
    else rt_set_cwd(p);
    RET(1, 1);
}
SHIM(KERNEL32, GetFullPathNameA) {
    const char *p = ARGP(0);
    char s[PATH_MAX];
    if (p[1] == ':') snprintf(s, sizeof s, "%s", p);
    else {
        const char *c = rt_get_cwd();
        snprintf(s, sizeof s, "C:\\" RT_VIRTUAL_INSTALL_DIR "%s%s\\%s", c[0] ? "\\" : "", c, p);
    }
    for (char *q = s; *q; q++) if (*q == '/') *q = '\\';
    uint32_t n = ARG(1), buf = ARG(2), fp = ARG(3);
    uint32_t len = (uint32_t)strlen(s);
    if (n <= len) RET(len + 1, 4);
    strcpy((char *)gptr(buf), s);
    if (fp) { char *b = strrchr((char *)gptr(buf), '\\'); g_wr32(fp, b ? buf + (uint32_t)(b + 1 - (char *)gptr(buf)) : buf); }
    RET(len, 4);
}
SHIM(KERNEL32, GetDriveTypeA) {
    const char *p = ARG(0) ? ARGP(0) : "C:\\";
    TRACE("GetDriveTypeA(%s)", p);
    // pretend D: is the CD-ROM containing the game disc (some builds check for the CD)
    if ((p[0] | 32) == 'd') RET(5, 1);
    RET(3, 1);   // DRIVE_FIXED
}

// ------------------------------------------------------------------ CoD additions
SHIM(KERNEL32, CreateToolhelp32Snapshot) { RET(INVALID_HANDLE, 2); }
SHIM(KERNEL32, Module32First) { RET(0, 2); }
SHIM(KERNEL32, Module32Next) { RET(0, 2); }
SHIM(KERNEL32, DeleteFileA) {
    char host[PATH_MAX];
    rt_map_path(ARGP(0), host, sizeof host);
    RET(unlink(host) == 0, 1);
}
SHIM(KERNEL32, MoveFileA) {
    char a[PATH_MAX], b[PATH_MAX];
    rt_map_path(ARGP(0), a, sizeof a);
    rt_map_path(ARGP(1), b, sizeof b);
    mkparents(b);
    RET(rename(a, b) == 0, 2);
}
SHIM(KERNEL32, GetCurrentProcessId) { RET(0x42, 0); }
SHIM(KERNEL32, OpenProcess) { RET(0, 3); }
static void fmt_time_to(uint32_t st, const char *fmt, uint32_t out, uint32_t cap, int is_time) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (st) {
        t.tm_year = g_rd16(st) - 1900; t.tm_mon = g_rd16(st + 2) - 1; t.tm_mday = g_rd16(st + 6);
        t.tm_hour = g_rd16(st + 8); t.tm_min = g_rd16(st + 10); t.tm_sec = g_rd16(st + 12);
    }
    char buf[128];
    (void)fmt;
    strftime(buf, sizeof buf, is_time ? "%H:%M:%S" : "%m/%d/%Y", &t);
    uint32_t n = (uint32_t)strlen(buf) + 1;
    if (cap && out) { strncpy((char *)gptr(out), buf, cap); gptr(out)[cap - 1] = 0; }
    rt_cpu()->eax = cap ? (n < cap ? n : cap) : n;
}
SHIM(KERNEL32, GetDateFormatA) { fmt_time_to(ARG(2), NULL, ARG(4), ARG(5), 0); RET(cpu->eax, 6); }
SHIM(KERNEL32, GetTimeFormatA) { fmt_time_to(ARG(2), NULL, ARG(4), ARG(5), 1); RET(cpu->eax, 6); }
SHIM(KERNEL32, GetSystemDirectoryA) {
    const char *s = "C:\\WINDOWS\\system32";
    if (ARG(1) > strlen(s)) strcpy(ARGP(0), s);
    RET(strlen(s), 2);
}
SHIM(KERNEL32, GetSystemInfo) {
    uint8_t *d = gptr(ARG(0));
    memset(d, 0, 36);
    *(uint32_t *)(d + 4) = 4096;           // dwPageSize
    *(uint32_t *)(d + 8) = 0x10000;        // lpMinimumApplicationAddress
    *(uint32_t *)(d + 12) = 0x7FFEFFFF;
    *(uint32_t *)(d + 16) = 1;
    *(uint32_t *)(d + 20) = 1;             // one CPU: keep the renderer single-threaded
    *(uint32_t *)(d + 24) = 586;           // PROCESSOR_INTEL_PENTIUM
    *(uint32_t *)(d + 28) = 0x10000;       // allocation granularity
    *(uint16_t *)(d + 32) = 6;
    RET(0, 1);
}
SHIM(KERNEL32, GetSystemTimeAsFileTime) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
    g_wr32(ARG(0), (uint32_t)ft); g_wr32(ARG(0) + 4, (uint32_t)(ft >> 32));
    RET(0, 1);
}
SHIM(KERNEL32, GetTempPathA) {
    const char *s = "C:\\" RT_VIRTUAL_INSTALL_DIR "\\temp\\";
    if (ARG(0) > strlen(s)) strcpy(ARGP(1), s);
    RET(strlen(s), 2);
}
SHIM(KERNEL32, GetTempFileNameA) {   // (path, prefix, unique, out)
    static uint32_t n = 1;
    snprintf(ARGP(3), 260, "%s%.3s%04X.tmp", ARGP(0), ARGP(1), n++);
    RET(n, 4);
}
SHIM(KERNEL32, GlobalMemoryStatus) {   // report 1 GB physical / 2 GB virtual (fits in 32-bit fields)
    uint8_t *d = gptr(ARG(0));
    uint32_t v[8] = {32, 20, 0x40000000u, 0x30000000u, 0x80000000u, 0x70000000u, 0x7FFE0000u, 0x60000000u};
    memcpy(d, v, 32);
    RET(0, 1);
}
SHIM(KERNEL32, GlobalSize) { RET(gsize(ARG(0)), 1); }
SHIM(KERNEL32, MulDiv) {
    int64_t r = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    int32_t d = (int32_t)ARG(2);
    RET(d ? (uint32_t)(int32_t)((r + (r >= 0 ? d / 2 : -d / 2)) / d) : 0xFFFFFFFFu, 3);
}
SHIM(KERNEL32, SetEnvironmentVariableW) { RET(1, 2); }
SHIM(KERNEL32, SetErrorMode) { RET(0, 1); }
SHIM(KERNEL32, SetThreadExecutionState) { RET(0x80000000u, 1); }
SHIM(KERNEL32, VirtualProtect) { if (ARG(3)) g_wr32(ARG(3), 4); RET(1, 4); }
SHIM(KERNEL32, VirtualQuery) {   // (addr, MEMORY_BASIC_INFORMATION*, len)
    uint32_t a = ARG(0) & ~0xfffu, p = ARG(1);
    g_wr32(p, a); g_wr32(p + 4, a & ~0xffffu); g_wr32(p + 8, 4); g_wr32(p + 12, 0x10000);
    g_wr32(p + 16, 0x1000); g_wr32(p + 20, 4); g_wr32(p + 24, 0x20000);
    RET(28, 3);
}
SHIM(KERNEL32, DebugBreak) { LOG("DebugBreak() ignored"); RET(0, 0); }
