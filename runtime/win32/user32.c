// USER32 shims: one top-level window backed by the SDL window; messages from the platform queue.
#include "../rt.h"
#include "../platform.h"
#include <string.h>
#include <strings.h>
#include <time.h>

static uint32_t g_wndproc;           // guest WNDPROC of the registered class
static uint32_t g_class_style;
static uint32_t g_hwnd_next = 0x10010;
static uint32_t g_capture, g_focus;
static uint32_t g_wnd_userdata[16];
static int g_cursor_count;

#define WM_CREATE 0x0001
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_PAINT 0x000F
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C

// Window procedures. Everything the platform layer posts (hwnd 0 / g_main_hwnd, which is a 1x1 dummy) goes to
// g_wndproc: the proc of the last class registered before the first visible top-level window (the game window).
// Hidden helper windows created after that (libwww's "AsyncWindowClass" socket/timer window, used by
// cl_wwwDownload) keep their own class proc, and their RegisterClassA must not steal g_wndproc from the game.
typedef struct { char name[64]; uint32_t atom, proc; } WndClass;
static WndClass g_classes[16];
static int g_nclasses;
static int g_wndproc_locked;
typedef struct { uint32_t hwnd, proc; } OwnWnd;
static OwnWnd g_own[8];
static int g_nown;

static uint32_t own_proc(uint32_t hwnd) {
    for (int i = 0; i < g_nown; i++) if (g_own[i].hwnd == hwnd) return g_own[i].proc;
    return 0;
}
uint32_t user_send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t proc = own_proc(hwnd);
    if (!proc) proc = g_wndproc;
    if (!proc) return 0;
    Cpu *cpu = rt_cpu();
    return call_guest(cpu, proc, 0, 4, hwnd, msg, wp, lp);
}

static uint32_t class_proc(uint32_t name_or_atom) {
    for (int i = g_nclasses - 1; i >= 0; i--) {
        if (name_or_atom < 0x10000) { if (g_classes[i].atom == name_or_atom) return g_classes[i].proc; }
        else if (!strcasecmp(g_classes[i].name, (const char *)gptr(name_or_atom))) return g_classes[i].proc;
    }
    return 0;
}

SHIM(USER32, RegisterClassA) {
    uint32_t wc = ARG(0), proc = g_rd32(wc + 4), name = g_rd32(wc + 36);
    uint32_t atom = 0xC001 + (uint32_t)g_nclasses;
    if (g_nclasses < (int)(sizeof g_classes / sizeof *g_classes)) {
        WndClass *c = &g_classes[g_nclasses++];
        snprintf(c->name, sizeof c->name, "%s", name >= 0x10000 ? (char *)gptr(name) : "");
        c->atom = atom; c->proc = proc;
    }
    if (!g_wndproc_locked) { g_class_style = g_rd32(wc); g_wndproc = proc; }
    TRACE("RegisterClassA(%s) wndproc=%08x", name >= 0x10000 ? (char *)gptr(name) : "?", proc);
    RET(atom, 1);
}
SHIM(USER32, UnregisterClassA) { RET(1, 2); }
SHIM(USER32, CreateWindowExA) {
    uint32_t title = ARG(2), style = ARG(3);
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h = (int)ARG(7);
    uint32_t hwnd = g_hwnd_next;
    g_hwnd_next += 0x10;
    if (!g_main_hwnd) g_main_hwnd = hwnd;
    uint32_t proc = class_proc(ARG(1));
    if (proc && !(style & 0x40000000u) && (style & 0x10000000u)) {   // visible top-level: the game window
        g_wndproc = proc;
        g_wndproc_locked = 1;
    } else if (proc && g_wndproc_locked && proc != g_wndproc && g_nown < (int)(sizeof g_own / sizeof *g_own)) {
        g_own[g_nown++] = (OwnWnd){hwnd, proc};   // helper window created after the game window
    }
    LOG("CreateWindowExA(%s, style=%08x, %d,%d %dx%d) -> %08x", title ? (char *)gptr(title) : "", style, x, y, w, h, hwnd);
    if (title && hwnd == g_main_hwnd) plat_set_title((char *)gptr(title));
    // the renderer's window (windowed r_mode: no ChangeDisplaySettings): adopt its client size as the display mode.
    // Top-level only; the splash (320x100) and the Win32 console (541x451) are smaller than any r_mode >= 640x480.
    if (!(style & 0x40000000u) && w >= 640 && h >= 480 && (w != plat_mode_w() || h != plat_mode_h())) plat_set_mode(w, h);
    if (hwnd == g_main_hwnd) {
        uint32_t cs = galloc(48);
        g_wr32(cs + 0, ARG(10)); g_wr32(cs + 4, ARG(8)); g_wr32(cs + 8, 0); g_wr32(cs + 12, ARG(9));
        g_wr32(cs + 16, h); g_wr32(cs + 20, w); g_wr32(cs + 24, y); g_wr32(cs + 28, x);
        g_wr32(cs + 32, style); g_wr32(cs + 36, title); g_wr32(cs + 40, ARG(1)); g_wr32(cs + 44, ARG(0));
        uint32_t esp = cpu->esp;
        user_send(hwnd, WM_CREATE, 0, cs);
        cpu->esp = esp;
    }
    RET(hwnd, 12);
}
SHIM(USER32, DestroyWindow) {
    for (int i = 0; i < g_nown; i++) if (g_own[i].hwnd == ARG(0)) { g_own[i] = g_own[--g_nown]; break; }
    RET(1, 1);
}
SHIM(USER32, ShowWindow) {
    uint32_t hwnd = ARG(0);
    if (hwnd == g_main_hwnd) {
        plat_post_msg(hwnd, WM_SHOWWINDOW, 1, 0);
        plat_post_msg(hwnd, WM_ACTIVATEAPP, 1, 0);
        plat_post_msg(hwnd, WM_ACTIVATE, 1, 0);
        plat_post_msg(hwnd, WM_SETFOCUS, 0, 0);
    }
    RET(1, 2);
}
SHIM(USER32, UpdateWindow) { RET(1, 1); }
SHIM(USER32, SetFocus) { uint32_t o = g_focus; g_focus = ARG(0); RET(o, 1); }
SHIM(USER32, GetFocus) { RET(g_focus ? g_focus : g_main_hwnd, 0); }
SHIM(USER32, SetActiveWindow) { RET(g_main_hwnd, 1); }
SHIM(USER32, SetForegroundWindow) { RET(1, 1); }
SHIM(USER32, FindWindowA) { RET(0, 2); }
SHIM(USER32, GetDesktopWindow) { RET(0x10000, 0); }
SHIM(USER32, GetWindowLongA) {
    int32_t i = (int32_t)ARG(1);
    if (i == -4) RET(g_wndproc, 2);        // GWL_WNDPROC
    if (i == -21) RET(g_wnd_userdata[0], 2);
    if (i == -16) RET(0x10000000 | 0x80000000, 2);   // WS_VISIBLE|WS_POPUP
    RET(0, 2);
}
SHIM(USER32, SetWindowLongA) {
    int32_t i = (int32_t)ARG(1);
    uint32_t old = 0;
    if (i == -4) { old = g_wndproc; g_wndproc = ARG(2); }
    else if (i == -21) { old = g_wnd_userdata[0]; g_wnd_userdata[0] = ARG(2); }
    RET(old, 3);
}
static void put_rect(uint32_t r, int l, int t, int rr, int b) { g_wr32(r, l); g_wr32(r + 4, t); g_wr32(r + 8, rr); g_wr32(r + 12, b); }
SHIM(USER32, GetWindowRect) { put_rect(ARG(1), 0, 0, plat_mode_w(), plat_mode_h()); RET(1, 2); }
SHIM(USER32, GetClientRect) { put_rect(ARG(1), 0, 0, plat_mode_w(), plat_mode_h()); RET(1, 2); }
SHIM(USER32, AdjustWindowRect) { RET(1, 3); }
SHIM(USER32, AdjustWindowRectEx) { RET(1, 4); }
SHIM(USER32, MoveWindow) { RET(1, 6); }
SHIM(USER32, SetWindowPos) { RET(1, 7); }
SHIM(USER32, GetMenu) { RET(0, 1); }
SHIM(USER32, ClientToScreen) { RET(1, 2); }
SHIM(USER32, ScreenToClient) { RET(1, 2); }
SHIM(USER32, MapWindowPoints) { RET(0, 4); }
SHIM(USER32, WindowFromPoint) { RET(g_main_hwnd, 2); }   // POINT by value = 2 dwords
SHIM(USER32, InvalidateRect) { RET(1, 3); }
SHIM(USER32, BeginPaint) { memset(gptr(ARG(1)), 0, 64); RET(0x20001, 2); }
SHIM(USER32, EndPaint) { RET(1, 2); }
SHIM(USER32, GetDC) { RET(0x20001, 1); }
SHIM(USER32, ReleaseDC) { RET(1, 2); }
SHIM(USER32, FillRect) { RET(1, 3); }
SHIM(USER32, SetRect) { put_rect(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); RET(1, 5); }
SHIM(USER32, SystemParametersInfoA) {
    uint32_t action = ARG(0), pv = ARG(2);
    if (action == 0x30 && pv) put_rect(pv, 0, 0, 1920, 1080);   // SPI_GETWORKAREA
    else if (pv && (action == 0x10 || action == 0x61 || action == 0x3A)) g_wr32(pv, 0);
    RET(1, 4);
}
SHIM(USER32, LoadCursorA) { RET(0x30001, 2); }
SHIM(USER32, LoadIconA) { RET(0x30002, 2); }
SHIM(USER32, SetCursor) { RET(0x30001, 1); }
SHIM(USER32, ShowCursor) {
    g_cursor_count += ARG(0) ? 1 : -1;
    plat_show_cursor(g_cursor_count >= 0);   // hidden cursor == game owns the mouse (relative mode)
    RET(g_cursor_count, 1);
}
SHIM(USER32, SetCursorPos) { plat_warp_mouse((int)ARG(0), (int)ARG(1)); RET(1, 2); }
SHIM(USER32, GetCursorPos) { g_wr32(ARG(0), g_mouse_x); g_wr32(ARG(0) + 4, g_mouse_y); RET(1, 1); }
SHIM(USER32, SetCapture) { uint32_t o = g_capture; g_capture = ARG(0); RET(o, 1); }
SHIM(USER32, ReleaseCapture) { g_capture = 0; RET(1, 0); }
SHIM(USER32, GetCapture) { RET(g_capture, 0); }
SHIM(USER32, GetDoubleClickTime) { RET(500, 0); }
SHIM(USER32, GetKeyState) {
    uint32_t vk = ARG(0) & 0xff;
    RET((g_vk_down[vk] ? 0xFF80 : 0) | g_vk_toggle[vk], 1);
}
SHIM(USER32, GetAsyncKeyState) {
    uint32_t vk = ARG(0) & 0xff;
    uint32_t r = (g_vk_down[vk] ? 0x8000 : 0) | (g_vk_pressed_since[vk] ? 1 : 0);
    g_vk_pressed_since[vk] = 0;
    RET(r, 1);
}

// ------------------------------------------------------------------ messages
static void write_msg(uint32_t p, const Msg *m) {
    g_wr32(p, m->hwnd); g_wr32(p + 4, m->msg); g_wr32(p + 8, m->wp); g_wr32(p + 12, m->lp);
    g_wr32(p + 16, m->time); g_wr32(p + 20, m->x); g_wr32(p + 24, m->y);
}
// Timers (SetTimer) for helper windows: WM_TIMER is synthesized when the queue is polled, at most one pending
// per timer, like Windows. Timers on the legacy windows (the console's 1 s blink timer) stay inert as before.
#define WM_TIMER 0x0113
typedef struct { uint32_t hwnd, id, ms, proc, due; int pending; } Timer;
static Timer g_timers[16];
static int g_ntimers;
static void timers_poll(void) {
    if (!g_ntimers) return;
    uint32_t now = plat_time_ms();
    for (int i = 0; i < g_ntimers; i++) {
        Timer *t = &g_timers[i];
        if (t->pending || (int32_t)(now - t->due) < 0) continue;
        t->pending = 1;
        t->due = now + t->ms;
        plat_post_msg(t->hwnd, WM_TIMER, t->id, t->proc);
    }
}
static void timer_delivered(uint32_t hwnd, uint32_t id) {
    for (int i = 0; i < g_ntimers; i++) if (g_timers[i].hwnd == hwnd && g_timers[i].id == id) g_timers[i].pending = 0;
}
void wsa_async_poll(void);   // wsock32.c: WSAAsyncSelect notifications
static void poll_sources(void) {
    plat_pump();
    timers_poll();
    wsa_async_poll();
}

SHIM(USER32, PeekMessageA) {
    uint32_t p = ARG(0);
    poll_sources();
    Msg m;
    if (plat_peek_msg(&m, ARG(2), ARG(3), ARG(4) & 1)) { write_msg(p, &m); RET(1, 5); }
    RET(0, 5);
}
int rt_handle_try(uint32_t h);
void rt_checkpoint(void);
extern int rt_is_main_thread(void);
SHIM(USER32, MsgWaitForMultipleObjects) {
    // (nCount, pHandles, fWaitAll, dwMilliseconds, dwWakeMask): poll handles (and messages on the UI thread)
    uint32_t n = ARG(0), arr = ARG(1), all = ARG(2), ms = ARG(3), mask = ARG(4), t0 = plat_time_ms();
    int main_thread = rt_is_main_thread();
    rt_checkpoint();
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n && ok; i++) ok = rt_handle_try(g_rd32(arr + 4 * i));
            if (ok && n) RET(0, 5);
        } else {
            for (uint32_t i = 0; i < n; i++) if (rt_handle_try(g_rd32(arr + 4 * i))) RET(i, 5);
        }
        if (main_thread && mask) {
            poll_sources();
            Msg m;
            if (plat_peek_msg(&m, 0, 0, 0)) RET(n, 5);    // WAIT_OBJECT_0 + nCount
        }
        if (ms != 0xFFFFFFFFu && plat_time_ms() - t0 >= ms) RET(0x102, 5);
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
        rt_checkpoint();
    }
}
SHIM(USER32, TranslateMessage) { RET(0, 1); }
SHIM(USER32, DispatchMessageA) {
    uint32_t p = ARG(0);
    uint32_t hwnd = g_rd32(p), msg = g_rd32(p + 4), wp = g_rd32(p + 8), lp = g_rd32(p + 12);
    if (msg == WM_TIMER) {
        timer_delivered(hwnd, wp);
        if (lp) RET(call_guest(cpu, lp, 0, 4, hwnd, msg, wp, plat_time_ms()), 1);   // TIMERPROC
    }
    uint32_t r = user_send(hwnd, msg, wp, lp);
    RET(r, 1);
}
SHIM(USER32, SendMessageA) {
    uint32_t hwnd = ARG(0), msg = ARG(1);
    if (msg == 0x00C2 && ARG(3)) {   // EM_REPLACESEL into the engine's console edit box: mirror to stdout
        const char *t = ARGP(3);
        for (; *t; t++) if (*t != '\r') fputc(*t, stdout);
        fflush(stdout);
        RET(0, 4);
    }
    if (hwnd != g_main_hwnd && hwnd != 0) RET(0, 4);   // child controls of the console window: no-op
    RET(user_send(hwnd, msg, ARG(2), ARG(3)), 4);
}
SHIM(USER32, DefWindowProcA) {
    uint32_t msg = ARG(1);
    if (msg == 0x0010) { plat_post_msg(ARG(0), 0x0002, 0, 0); }   // WM_CLOSE -> WM_DESTROY
    RET(0, 4);
}
SHIM(USER32, PostQuitMessage) { plat_post_msg(0, WM_QUIT, ARG(0), 0); RET(0, 1); }
SHIM(USER32, MessageBoxA) {
    LOG("MessageBox [%s]: %s", ARG(2) ? ARGP(2) : "", ARG(1) ? ARGP(1) : "");
    uint32_t type = ARG(3) & 0xf;
    if (type == 3 || type == 4) RET(7, 4);   // MB_YESNO(CANCEL): IDNO (e.g. decline "run in safe mode")
    RET(1, 4);   // IDOK
}
SHIM(USER32, wsprintfA) {
    // cdecl varargs: (buf, fmt, ...) — supports %d %u %x %X %s %c %% with width/flags
    uint32_t buf = ARG(0);
    const char *f = ARGP(1);
    char out[1024]; size_t o = 0;
    int ai = 2;
    while (*f && o < sizeof out - 64) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[16]; int k = 0; spec[k++] = *f++;
        while (*f && strchr("-+ #0123456789.l", *f) && k < 12) { if (*f != 'l') spec[k++] = *f; f++; }
        char c = *f++;
        spec[k++] = c; spec[k] = 0;
        if (c == '%') { out[o++] = '%'; continue; }
        if (c == 's') o += snprintf(out + o, sizeof out - o, spec, (char *)gptr(ARG(ai++)));
        else o += snprintf(out + o, sizeof out - o, spec, ARG(ai++));
    }
    out[o] = 0;
    strcpy((char *)gptr(buf), out);
    RET_CDECL(o);
}
// dialogs / clipboard: developer tools only
SHIM(USER32, CreateDialogParamA) { RET(0, 5); }
SHIM(USER32, IsDialogMessageA) { RET(0, 2); }
SHIM(USER32, EndDialog) { RET(1, 2); }
SHIM(USER32, GetDlgItem) { RET(0, 2); }
SHIM(USER32, SendDlgItemMessageA) { RET(0, 5); }
SHIM(USER32, CheckDlgButton) { RET(1, 3); }
SHIM(USER32, CheckRadioButton) { RET(1, 4); }
SHIM(USER32, IsDlgButtonChecked) { RET(0, 2); }
SHIM(USER32, GetDlgItemTextA) { if (ARG(3)) gptr(ARG(2))[0] = 0; RET(0, 4); }
SHIM(USER32, SetDlgItemTextA) { RET(1, 3); }
SHIM(USER32, OpenClipboard) { RET(0, 1); }
SHIM(USER32, CloseClipboard) { RET(1, 0); }
SHIM(USER32, EmptyClipboard) { RET(1, 0); }
SHIM(USER32, GetClipboardData) { RET(0, 1); }
SHIM(USER32, SetClipboardData) { RET(0, 2); }

// ------------------------------------------------------------------ CoD additions: display modes, cursor, misc
int plat_desktop_w(void);
int plat_desktop_h(void);
int rt_vidmode_size(int i, int *w, int *h);
SHIM(USER32, CallWindowProcA) { RET(call_guest(cpu, ARG(0), 0, 4, ARG(1), ARG(2), ARG(3), ARG(4)), 5); }
// DEVMODEA: dmFields@40 dmBitsPerPel@104 dmPelsWidth@108 dmPelsHeight@112 dmDisplayFrequency@120, size 156
static void fill_devmode(uint32_t p, int w, int h) {
    uint8_t *d = gptr(p);
    memset(d, 0, 156);
    *(uint16_t *)(d + 36) = 156;
    *(uint32_t *)(d + 40) = 0x40000 | 0x80000 | 0x100000 | 0x400000;
    *(uint32_t *)(d + 104) = 32; *(uint32_t *)(d + 108) = (uint32_t)w; *(uint32_t *)(d + 112) = (uint32_t)h;
    *(uint32_t *)(d + 120) = 60;
}
SHIM(USER32, EnumDisplaySettingsA) {   // (device, iModeNum, devmode)
    // same list as r_mode (runtime/overrides/vidmodes.c), last entry = native desktop size
    int32_t i = (int32_t)ARG(1);
    int w, h;
    if (i == -1 || i == -2) { fill_devmode(ARG(2), plat_desktop_w(), plat_desktop_h()); RET(1, 3); }
    if (!rt_vidmode_size(i, &w, &h)) RET(0, 3);
    fill_devmode(ARG(2), w, h);
    RET(1, 3);
}
SHIM(USER32, ChangeDisplaySettingsA) {   // (devmode or NULL, flags)
    uint32_t dm = ARG(0);
    if (!dm) { plat_set_fullscreen(0); RET(0, 2); }
    if (ARG(1) & 2) RET(0, 2);   // CDS_TEST
    int w = (int)g_rd32(dm + 108), h = (int)g_rd32(dm + 112);
    LOG("ChangeDisplaySettings %dx%d", w, h);
    plat_set_mode(w, h);
    plat_set_fullscreen(1);
    RET(0, 2);   // DISP_CHANGE_SUCCESSFUL
}
SHIM(USER32, GetSystemMetrics) {
    switch ((int32_t)ARG(0)) {
    case 0: RET(plat_desktop_w(), 1);   // SM_CXSCREEN
    case 1: RET(plat_desktop_h(), 1);
    case 75: RET(1, 1);                 // SM_MOUSEWHEELPRESENT
    case 19: RET(1, 1);                 // SM_MOUSEPRESENT
    case 43: RET(3, 1);                 // SM_CMOUSEBUTTONS
    }
    RET(0, 1);
}
SHIM(USER32, ClipCursor) { plat_show_cursor(ARG(0) == 0 && g_cursor_count >= 0); RET(1, 1); }
SHIM(USER32, CloseWindow) { RET(1, 1); }
SHIM(USER32, GetForegroundWindow) { RET(g_main_hwnd, 0); }
SHIM(USER32, GetMessageA) {
    for (;;) {
        poll_sources();
        Msg m;
        if (plat_peek_msg(&m, ARG(2), ARG(3), 1)) { write_msg(ARG(0), &m); RET(m.msg != WM_QUIT, 4); }
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
}
SHIM(USER32, GetWindowTextA) { if (ARG(2)) ARGP(1)[0] = 0; RET(0, 3); }
SHIM(USER32, SetWindowTextA) { if (ARG(1) && ARG(0) == g_main_hwnd) plat_set_title(ARGP(1)); RET(1, 2); }
SHIM(USER32, LoadImageA) { RET(0x30003, 6); }
SHIM(USER32, MapVirtualKeyA) { RET(0, 2); }
SHIM(USER32, RegisterHotKey) { RET(1, 4); }
SHIM(USER32, UnregisterHotKey) { RET(1, 2); }
SHIM(USER32, RegisterWindowMessageA) {
    static uint32_t next = 0xC100;
    LOG("RegisterWindowMessageA(%s) -> %04x", ARGP(0), next);
    RET(next++, 1);
}
SHIM(USER32, SetTimer) {   // (hwnd, id, ms, timerproc)
    uint32_t hwnd = ARG(0), id = ARG(1) ? ARG(1) : 1, ms = ARG(2) < 10 ? 10 : ARG(2);
    if (!own_proc(hwnd)) RET(id, 4);
    Timer *t = NULL;
    for (int i = 0; i < g_ntimers; i++) if (g_timers[i].hwnd == hwnd && g_timers[i].id == id) t = &g_timers[i];
    if (!t && g_ntimers < (int)(sizeof g_timers / sizeof *g_timers)) t = &g_timers[g_ntimers++];
    if (!t) RET(0, 4);
    *t = (Timer){hwnd, id, ms, ARG(3), plat_time_ms() + ms, 0};
    RET(id, 4);
}
SHIM(USER32, KillTimer) {
    for (int i = 0; i < g_ntimers; i++)
        if (g_timers[i].hwnd == ARG(0) && g_timers[i].id == ARG(1)) { g_timers[i] = g_timers[--g_ntimers]; break; }
    RET(1, 2);
}
SHIM(USER32, EnumThreadWindows) { RET(1, 3); }   // (thread, proc, lParam): no other thread windows to report
