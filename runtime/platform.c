// Platform layer on SDL3: window with a CAMetalLayer (drawn by gl/), events -> Win32 messages, audio device.
#include "rt.h"
#include "platform.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <pthread.h>
#include <math.h>

static SDL_Window *g_win;
static SDL_MetalView g_view;
static int g_w = 640, g_h = 480;          // guest display mode (pixels)
static SDL_AudioStream *g_audio;
static void (*g_mix)(int16_t *, int);
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_relative;                    // game hid the cursor: feed relative motion

uint8_t g_vk_down[256], g_vk_toggle[256], g_vk_pressed_since[256];
int g_mouse_x, g_mouse_y;
int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;
uint8_t g_mouse_btn[4];
uint8_t g_dik_down[256];
uint32_t g_main_hwnd;
int g_quit_requested;

#define QCAP 1024
static Msg g_q[QCAP];
static int g_qh, g_qt;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

#define DICAP 256
static DIEvt g_die[DICAP];
static int g_dih, g_dit;
static uint32_t g_diseq;

void rt_platform_init(int argc, char **argv) {
    (void)argc; (void)argv;
    plat_mac_init();                                         // before SDL creates NSApp: no press-and-hold popup
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");     // first click into the window is not swallowed
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        LOG("SDL_Init failed: %s", SDL_GetError());
        exit(1);
    }
}

uint32_t plat_time_ms(void) { return (uint32_t)SDL_GetTicks(); }
int plat_mode_w(void) { return g_w; }
int plat_mode_h(void) { return g_h; }

static void window_points_for_mode(int *ww, int *wh);
static void ensure_window(void) {
    if (g_win) return;
    int ww, wh;
    window_points_for_mode(&ww, &wh);
    if (ww < 640 && g_w >= 640) { ww = 640; wh = 640 * g_h / g_w; }
    g_win = SDL_CreateWindow("Call of Duty", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
    g_view = SDL_Metal_CreateView(g_win);
    // no SDL/Cocoa text input: it routes keys through the macOS input system, which pops up the accent menu when a
    // key is held. WM_CHAR is generated from key events instead (see plat_pump).
    SDL_StopTextInput(g_win);
    // launched from a terminal the process is not frontmost: activate it so the mouse is captured immediately
    SDL_ShowWindow(g_win);
    SDL_RaiseWindow(g_win);
    plat_mac_activate();
    SDL_PumpEvents();   // FOCUS_GAINED (-> WM_ACTIVATE) arrives through plat_pump once the window is key
    LOG("window %dx%d points for mode %dx%d", ww, wh, g_w, g_h);
}

void *plat_metal_layer(void) { ensure_window(); return SDL_Metal_GetLayer(g_view); }
void plat_drawable_size(int *w, int *h) { ensure_window(); SDL_GetWindowSizeInPixels(g_win, w, h); }

// window size in points for a mode in pixels: 1 mode pixel per display pixel (Retina), shrunk to fit the usable
// desktop area with the mode's aspect ratio preserved (the drawable letterboxes anyway)
static void window_points_for_mode(int *ww, int *wh) {
    SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(d);
    float sc = dm && dm->pixel_density > 0 ? dm->pixel_density : 1.0f;
    double w = g_w / sc, h = g_h / sc;
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(d, &r) && r.w > 0 && r.h > 0) {
        double maxh = r.h - 32.0;   // title bar
        double f = fmin(1.0, fmin(r.w / w, maxh / h));
        w *= f; h *= f;
    }
    *ww = (int)(w + 0.5); *wh = (int)(h + 0.5);
}

void plat_set_mode(int w, int h) {
    g_w = w; g_h = h;
    if (g_win && !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)) {
        int ww, wh;
        window_points_for_mode(&ww, &wh);
        SDL_SetWindowSize(g_win, ww, wh);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    ensure_window();
    LOG("display mode %dx%d", w, h);
}

void plat_set_fullscreen(int fs) { ensure_window(); SDL_SetWindowFullscreen(g_win, fs ? true : false); }
void plat_set_title(const char *s) { ensure_window(); SDL_SetWindowTitle(g_win, s); }
void plat_show_cursor(int show) {
    ensure_window();
    int rel = !show;
    if (rel != g_relative) {
        g_relative = rel;
        SDL_SetWindowRelativeMouseMode(g_win, rel ? true : false);
    }
}
// Game-coordinate cursor: SetCursorPos re-centres it; relative motion moves it (Quake-style mouse delta polling).
void plat_warp_mouse(int x, int y) { g_mouse_x = x; g_mouse_y = y; }

// window points -> guest pixels
static void to_game(float x, float y, int *gx, int *gy) {
    int ww = 1, wh = 1;
    SDL_GetWindowSize(g_win, &ww, &wh);
    *gx = (int)(x * g_w / (ww ? ww : 1));
    *gy = (int)(y * g_h / (wh ? wh : 1));
}

void plat_dump_rgba(const uint8_t *pix, int w, int h, int flip_y, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rowsz = (uint32_t)w * 3, pad = (4 - rowsz % 4) % 4, img = (rowsz + pad) * h;
    uint8_t hdr[54] = {'B', 'M'};
    *(uint32_t *)(hdr + 2) = 54 + img; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w; *(int32_t *)(hdr + 22) = h; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = img;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, rowsz + pad);
    for (int yy = 0; yy < h; yy++) {
        int y = flip_y ? yy : h - 1 - yy;    // BMP rows bottom-up
        const uint8_t *src = pix + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[3 * x] = src[4 * x + 2]; row[3 * x + 1] = src[4 * x + 1]; row[3 * x + 2] = src[4 * x]; }
        fwrite(row, 1, rowsz + pad, f);
    }
    free(row);
    fclose(f);
}

// ---- scripted input for automated testing: COD_INPUT=file with lines "<ms> <action> [args]"
//   click X Y | rclick X Y | move X Y | key VK(hex or dec) | text STRING | dump
typedef struct { uint32_t t; char act[16]; int a, b; char str[96]; } ScriptEv;
static ScriptEv g_script[512];
static int g_nscript, g_script_pos, g_dump_req;
static void load_script(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *fn = getenv("COD_INPUT");
    if (!fn) return;
    FILE *f = fopen(fn, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && g_nscript < 512) {
        ScriptEv *e = &g_script[g_nscript];
        e->a = e->b = 0;
        e->str[0] = 0;
        if (line[0] == '#' || sscanf(line, "%u %15s %i %i", &e->t, e->act, &e->a, &e->b) < 2) continue;
        if (!strcmp(e->act, "text")) {   // rest of line after "text "
            char *q = strstr(line, "text ");
            if (q) { snprintf(e->str, sizeof e->str, "%s", q + 5); e->str[strcspn(e->str, "\r\n")] = 0; }
        }
        g_nscript++;
    }
    fclose(f);
    LOG("input script: %d events", g_nscript);
}
static void mouse_button(int b, int down);
static void run_script(void) {
    load_script();
    uint32_t now = plat_time_ms();
    while (g_script_pos < g_nscript && g_script[g_script_pos].t <= now) {
        ScriptEv *e = &g_script[g_script_pos++];
        LOG("script @%u: %s %d %d", now, e->act, e->a, e->b);
        if (!strcmp(e->act, "dump")) { g_dump_req = 1; continue; }
        if (!strcmp(e->act, "quit")) { exit(0); }
        if (!strcmp(e->act, "text")) { for (char *c = e->str; *c; c++) plat_post_msg(0, 0x102, (uint8_t)*c, 1); continue; }
        if (!strcmp(e->act, "keydown") || !strcmp(e->act, "keyup")) {   // hold/release: keydown VK SCANCODE
            int dn = e->act[3] == 'd';
            uint32_t vk = (uint32_t)e->a, sc = (uint32_t)e->b;
            g_vk_down[vk] = dn;
            plat_post_msg(0, dn ? 0x100 : 0x101, vk, dn ? (1 | (sc << 16)) : (0xC0000001u | (sc << 16)));
            continue;
        }
        if (!strcmp(e->act, "lbdown")) { mouse_button(SDL_BUTTON_LEFT, 1); continue; }
        if (!strcmp(e->act, "lbup")) { mouse_button(SDL_BUTTON_LEFT, 0); continue; }
        if (!strcmp(e->act, "key")) {
            uint32_t vk = (uint32_t)e->a;
            g_vk_down[vk] = 1; g_vk_pressed_since[vk] = 1;
            uint32_t sc = (uint32_t)e->b;   // optional PC scancode (Quake maps keys by scancode)
            plat_post_msg(0, 0x100, vk, 1 | (sc << 16));
            if (vk == 0x0D || vk == 0x1B || vk == 0x08) plat_post_msg(0, 0x102, vk, 1 | (sc << 16));
            plat_post_msg(0, 0x101, vk, 0xC0000001u | (sc << 16));
            g_vk_down[vk] = 0;
            continue;
        }
        int dx = e->a - g_mouse_x, dy = e->b - g_mouse_y;
        g_mouse_dx += dx; g_mouse_dy += dy;
        g_mouse_x = e->a; g_mouse_y = e->b;
        plat_post_msg(0, 0x200, 0, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
        if (!strcmp(e->act, "click")) { mouse_button(SDL_BUTTON_LEFT, 1); mouse_button(SDL_BUTTON_LEFT, 0); }
        else if (!strcmp(e->act, "down")) mouse_button(SDL_BUTTON_LEFT, 1);
        else if (!strcmp(e->act, "up")) mouse_button(SDL_BUTTON_LEFT, 0);
        else if (!strcmp(e->act, "rclick")) { mouse_button(SDL_BUTTON_RIGHT, 1); mouse_button(SDL_BUTTON_RIGHT, 0); }
    }
}

int plat_take_dump_request(void) { int r = g_dump_req; g_dump_req = 0; return r; }

// ------------------------------------------------------------------ messages
void plat_post_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qlock);
    int n = (g_qt + 1) % QCAP;
    if (n != g_qh) {
        g_q[g_qt] = (Msg){hwnd ? hwnd : g_main_hwnd, msg, wp, lp, plat_time_ms(), (uint32_t)g_mouse_x, (uint32_t)g_mouse_y};
        g_qt = n;
    }
    pthread_mutex_unlock(&g_qlock);
}

int plat_peek_msg(Msg *m, uint32_t fmin, uint32_t fmax, int remove) {
    pthread_mutex_lock(&g_qlock);
    for (int i = g_qh; i != g_qt; i = (i + 1) % QCAP) {
        Msg *x = &g_q[i];
        if (fmin || fmax) { if (x->msg < fmin || x->msg > fmax) continue; }
        *m = *x;
        if (remove) {
            // remove i by shifting
            for (int j = i; j != g_qh; ) { int p = (j - 1 + QCAP) % QCAP; g_q[j] = g_q[p]; j = p; }
            g_qh = (g_qh + 1) % QCAP;
        }
        pthread_mutex_unlock(&g_qlock);
        return 1;
    }
    pthread_mutex_unlock(&g_qlock);
    return 0;
}

int plat_mouse_event_pop(DIEvt *e) {
    if (g_dih == g_dit) return 0;
    *e = g_die[g_dih];
    g_dih = (g_dih + 1) % DICAP;
    return 1;
}
static void di_push(uint32_t ofs, uint32_t data) {
    int n = (g_dit + 1) % DICAP;
    if (n == g_dih) g_dih = (g_dih + 1) % DICAP;
    g_die[g_dit] = (DIEvt){ofs, data, plat_time_ms(), ++g_diseq};
    g_dit = n;
}

// SDL scancode -> (VK, PC set-1 scancode, extended)
static void map_key(SDL_Scancode sc, SDL_Keycode kc, uint32_t *vk, uint32_t *pcsc, int *ext) {
    *ext = 0; *pcsc = 0; *vk = 0;
    if (kc >= 'a' && kc <= 'z') *vk = (uint32_t)(kc - 32);
    else if (kc >= '0' && kc <= '9') *vk = (uint32_t)kc;
    static const struct { SDL_Scancode s; uint8_t vk, pc, ext; } T[] = {
        {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, 0}, {SDL_SCANCODE_RETURN, 0x0D, 0x1C, 0}, {SDL_SCANCODE_SPACE, 0x20, 0x39, 0},
        {SDL_SCANCODE_TAB, 0x09, 0x0F, 0}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, 0},
        {SDL_SCANCODE_LSHIFT, 0x10, 0x2A, 0}, {SDL_SCANCODE_RSHIFT, 0x10, 0x36, 0},
        {SDL_SCANCODE_LCTRL, 0x11, 0x1D, 0}, {SDL_SCANCODE_RCTRL, 0x11, 0x1D, 1},
        {SDL_SCANCODE_LALT, 0x12, 0x38, 0}, {SDL_SCANCODE_RALT, 0x12, 0x38, 1},
        {SDL_SCANCODE_LGUI, 0x11, 0x1D, 0}, {SDL_SCANCODE_RGUI, 0x11, 0x1D, 1},   // Cmd acts as Ctrl
        {SDL_SCANCODE_LEFT, 0x25, 0x4B, 1}, {SDL_SCANCODE_UP, 0x26, 0x48, 1}, {SDL_SCANCODE_RIGHT, 0x27, 0x4D, 1},
        {SDL_SCANCODE_DOWN, 0x28, 0x50, 1}, {SDL_SCANCODE_INSERT, 0x2D, 0x52, 1}, {SDL_SCANCODE_DELETE, 0x2E, 0x53, 1},
        {SDL_SCANCODE_HOME, 0x24, 0x47, 1}, {SDL_SCANCODE_END, 0x23, 0x4F, 1}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, 1},
        {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, 1}, {SDL_SCANCODE_PAUSE, 0x13, 0x45, 0}, {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A, 0},
        {SDL_SCANCODE_F1, 0x70, 0x3B, 0}, {SDL_SCANCODE_F2, 0x71, 0x3C, 0}, {SDL_SCANCODE_F3, 0x72, 0x3D, 0},
        {SDL_SCANCODE_F4, 0x73, 0x3E, 0}, {SDL_SCANCODE_F5, 0x74, 0x3F, 0}, {SDL_SCANCODE_F6, 0x75, 0x40, 0},
        {SDL_SCANCODE_F7, 0x76, 0x41, 0}, {SDL_SCANCODE_F8, 0x77, 0x42, 0}, {SDL_SCANCODE_F9, 0x78, 0x43, 0},
        {SDL_SCANCODE_F10, 0x79, 0x44, 0}, {SDL_SCANCODE_F11, 0x7A, 0x57, 0}, {SDL_SCANCODE_F12, 0x7B, 0x58, 0},
        {SDL_SCANCODE_KP_0, 0x60, 0x52, 0}, {SDL_SCANCODE_KP_1, 0x61, 0x4F, 0}, {SDL_SCANCODE_KP_2, 0x62, 0x50, 0},
        {SDL_SCANCODE_KP_3, 0x63, 0x51, 0}, {SDL_SCANCODE_KP_4, 0x64, 0x4B, 0}, {SDL_SCANCODE_KP_5, 0x65, 0x4C, 0},
        {SDL_SCANCODE_KP_6, 0x66, 0x4D, 0}, {SDL_SCANCODE_KP_7, 0x67, 0x47, 0}, {SDL_SCANCODE_KP_8, 0x68, 0x48, 0},
        {SDL_SCANCODE_KP_9, 0x69, 0x49, 0}, {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37, 0}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E, 0},
        {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A, 0}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53, 0}, {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0x35, 1},
        {SDL_SCANCODE_KP_ENTER, 0x0D, 0x1C, 1},
        {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, 0}, {SDL_SCANCODE_EQUALS, 0xBB, 0x0D, 0}, {SDL_SCANCODE_COMMA, 0xBC, 0x33, 0},
        {SDL_SCANCODE_MINUS, 0xBD, 0x0C, 0}, {SDL_SCANCODE_PERIOD, 0xBE, 0x34, 0}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, 0},
        {SDL_SCANCODE_GRAVE, 0xC0, 0x29, 0}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, 0}, {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, 0},
        {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, 0}, {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, 0},
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].s == sc) { *vk = T[i].vk; *pcsc = T[i].pc; *ext = T[i].ext; return; }
    static const uint8_t letters[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { *vk = 'A' + (sc - SDL_SCANCODE_A); *pcsc = letters[sc - SDL_SCANCODE_A]; }
    else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { *vk = '1' + (sc - SDL_SCANCODE_1); *pcsc = 0x02 + (sc - SDL_SCANCODE_1); }
    else if (sc == SDL_SCANCODE_0) { *vk = '0'; *pcsc = 0x0B; }
}

static void mouse_button(int b, int down) {
    static const uint32_t dn[3] = {0x201, 0x204, 0x207}, up[3] = {0x202, 0x205, 0x208};
    int i = b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1;
    if (i < 0) return;
    g_mouse_btn[i] = down ? 0x80 : 0;
    static const uint8_t vk[3] = {0x01, 0x02, 0x04};
    g_vk_down[vk[i]] = down;
    if (down) g_vk_pressed_since[vk[i]] = 1;
    uint32_t mk = (g_mouse_btn[0] ? 1 : 0) | (g_mouse_btn[1] ? 2 : 0) | (g_mouse_btn[2] ? 0x10 : 0) |
                  (g_vk_down[0x10] ? 4 : 0) | (g_vk_down[0x11] ? 8 : 0);
    plat_post_msg(0, down ? dn[i] : up[i], mk, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
    di_push(12 + i, down ? 0x80 : 0);   // DIMOFS_BUTTON0..
}

// Quit through the engine (WM_CLOSE -> "quit" -> CL_Disconnect), so a connected client tells the server it left;
// a hard exit leaves the player (and the CD key) "connected" until the server times out. A second request, or
// the engine not exiting within 5 s, forces the exit.
static uint32_t g_quit_time;
static void request_quit(void) {
    if (g_quit_requested) {   // one Cmd+Q can arrive twice (key event + SDL's app-menu quit): ignore for 1 s
        if (plat_time_ms() - g_quit_time < 1000) return;
        LOG("quit requested again: forcing exit");
        exit(0);
    }
    g_quit_requested = 1;
    g_quit_time = plat_time_ms();
    plat_post_msg(0, 0x0010, 0, 0);   // WM_CLOSE
}

void plat_pump(void) {
    SDL_Event e;
    if (g_quit_requested && g_quit_time && plat_time_ms() - g_quit_time > 5000) {
        LOG("engine did not exit 5 s after quit: forcing exit");
        exit(0);
    }
    run_script();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            request_quit();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint32_t vk, pc; int ext;
            map_key(e.key.scancode, e.key.key, &vk, &pc, &ext);
            if (!vk) break;
            int down = e.type == SDL_EVENT_KEY_DOWN;
            // Cmd+Q / Cmd+Enter: host shortcuts
            if (down && !e.key.repeat && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_Q) { request_quit(); break; }
            if (down && ((e.key.mod & SDL_KMOD_GUI) || (e.key.mod & SDL_KMOD_ALT)) && e.key.scancode == SDL_SCANCODE_RETURN) {
                SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (pc) g_dik_down[(pc | (ext ? 0x80 : 0)) & 0xff] = down ? 0x80 : 0;
            uint32_t prev = g_vk_down[vk];
            g_vk_down[vk] = down;
            if (down) { g_vk_pressed_since[vk] = 1; if (!e.key.repeat) g_vk_toggle[vk] ^= 1; }
            int alt = g_vk_down[0x12];
            uint32_t lp = 1 | (pc << 16) | ((uint32_t)ext << 24) | (alt ? 1u << 29 : 0) | (prev ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
            uint32_t msg = down ? (alt && vk != 0x12 ? 0x104 : 0x100) : (alt && vk != 0x12 ? 0x105 : 0x101);
            plat_post_msg(0, msg, vk, lp);
            // WM_CHAR for control keys that SDL text input does not deliver
            if (down && (vk == 0x08 || vk == 0x0D || vk == 0x1B || vk == 0x09)) plat_post_msg(0, 0x102, vk, lp);
            else if (down && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
                // printable character for the current layout and shift/caps state (console, save names)
                SDL_Keycode ch = SDL_GetKeyFromScancode(e.key.scancode, e.key.mod, false);
                if (ch >= 0x20 && ch < 0x7F) plat_post_msg(0, 0x102, (uint32_t)ch, lp);
                else if (ch >= 0xA0 && ch <= 0xFF) plat_post_msg(0, 0x102, (uint32_t)ch, lp);   // Latin-1
            }
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            for (const unsigned char *s = (const unsigned char *)e.text.text; *s; s++) {
                uint32_t c = *s;
                if (c >= 0xC0 && (s[1] & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (s[1] & 0x3F); s++; }   // 2-byte UTF-8 -> Latin-1
                else if (c >= 0x80) continue;
                plat_post_msg(0, 0x102, c, 1);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            int ww = 1, wh = 1;
            SDL_GetWindowSize(g_win, &ww, &wh);
            float sx = (float)g_w / (ww ? ww : 1), sy = (float)g_h / (wh ? wh : 1);
            static float fx, fy;   // sub-pixel remainder
            fx += e.motion.xrel * sx; fy += e.motion.yrel * sy;
            int dx = (int)fx, dy = (int)fy;
            fx -= dx; fy -= dy;
            g_mouse_dx += dx; g_mouse_dy += dy;
            if (dx) di_push(0, (uint32_t)dx);
            if (dy) di_push(4, (uint32_t)dy);
            if (g_relative) { g_mouse_x += dx; g_mouse_y += dy; }
            else to_game(e.motion.x, e.motion.y, &g_mouse_x, &g_mouse_y);
            int nx = g_mouse_x < 0 ? 0 : g_mouse_x >= g_w ? g_w - 1 : g_mouse_x;
            int ny = g_mouse_y < 0 ? 0 : g_mouse_y >= g_h ? g_h - 1 : g_mouse_y;
            uint32_t mk = (g_mouse_btn[0] ? 1 : 0) | (g_mouse_btn[1] ? 2 : 0) | (g_mouse_btn[2] ? 0x10 : 0);
            plat_post_msg(0, 0x200, mk, (uint32_t)(nx & 0xffff) | ((uint32_t)ny << 16));
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!g_relative) to_game(e.button.x, e.button.y, &g_mouse_x, &g_mouse_y);
            mouse_button(e.button.button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            int d = (int)(e.wheel.y * 120);
            g_mouse_dz += d;
            di_push(8, (uint32_t)d);
            plat_post_msg(0, 0x20A, (uint32_t)(d & 0xffff) << 16, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (g_relative) SDL_SetWindowRelativeMouseMode(g_win, true);   // SDL may drop it while unfocused
            plat_post_msg(0, 0x001C, 1, 0);       // WM_ACTIVATEAPP
            plat_post_msg(0, 0x0006, 1, 0);       // WM_ACTIVATE
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            memset(g_vk_down, 0, sizeof g_vk_down);
            plat_post_msg(0, 0x001C, 0, 0);
            plat_post_msg(0, 0x0006, 0, 0);
            break;
        }
    }
    if (g_win) rt_focus_sync((SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) != 0);
}

// ------------------------------------------------------------------ audio
static int g_rate;
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    int frames = additional / 4;
    if (frames <= 0) return;
    static int16_t buf[16384];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        pthread_mutex_lock(&g_audio_lock);
        g_mix(buf, n);
        pthread_mutex_unlock(&g_audio_lock);
        SDL_PutAudioStreamData(s, buf, n * 4);
        frames -= n;
    }
}
void plat_audio_start(void (*mix)(int16_t *, int), int rate) {
    if (g_audio) return;
    g_mix = mix; g_rate = rate;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, rate};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!g_audio) { LOG("audio open failed: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_audio);
    LOG("audio started %d Hz", rate);
}
void plat_audio_lock(void) { pthread_mutex_lock(&g_audio_lock); }
void plat_audio_unlock(void) { pthread_mutex_unlock(&g_audio_lock); }

// ------------------------------------------------------------------ desktop
int plat_desktop_w(void) {
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return dm ? (int)(dm->w * dm->pixel_density) : 1920;
}
int plat_desktop_h(void) {
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return dm ? (int)(dm->h * dm->pixel_density) : 1080;
}
