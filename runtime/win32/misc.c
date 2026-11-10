// GDI32 (pixel format, gamma), ADVAPI32, WINMM, SHELL32, DDRAW shims.
#include "../rt.h"
#include "../platform.h"
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

void gl_swap_buffers(void);                       // gl/ : present the frame
void gl_set_gamma_ramp(const uint16_t *ramp);     // gl/ : 3x256 ramp applied at present

// ------------------------------------------------------------------ GDI32
int plat_desktop_w(void);
int plat_desktop_h(void);
SHIM(GDI32, GetDeviceCaps) {
    switch (ARG(1)) {
    case 8: RET(plat_desktop_w(), 2);    // HORZRES
    case 10: RET(plat_desktop_h(), 2);   // VERTRES
    case 12: RET(32, 2);     // BITSPIXEL
    case 14: RET(1, 2);      // PLANES
    case 88: case 90: RET(96, 2);   // LOGPIXELSX/Y
    case 116: RET(60, 2);    // VREFRESH
    case 0x78: RET(1, 2);    // COLORMGMTCAPS: gamma ramps supported
    }
    RET(0, 2);
}
// One pixel format: RGBA8888, 24-bit depth, 8-bit stencil, double-buffered OpenGL.
static void fill_pfd(uint32_t p) {
    uint8_t *d = gptr(p);
    memset(d, 0, 40);
    *(uint16_t *)d = 40; *(uint16_t *)(d + 2) = 1;
    *(uint32_t *)(d + 4) = 0x4 | 0x20 | 0x1;    // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
    d[8] = 0;                                    // PFD_TYPE_RGBA
    d[9] = 32; d[10] = 8; d[11] = 16; d[12] = 8; d[13] = 8; d[14] = 8; d[15] = 0; d[16] = 8; d[17] = 24;
    d[23] = 24; d[24] = 8;                       // depth, stencil
}
SHIM(GDI32, ChoosePixelFormat) { RET(1, 2); }
SHIM(GDI32, DescribePixelFormat) {   // (hdc, iPixelFormat, nBytes, ppfd) -> max index
    if (ARG(3) && ARG(2) >= 40) fill_pfd(ARG(3));
    RET(1, 4);
}
SHIM(GDI32, SetPixelFormat) { RET(1, 3); }
SHIM(GDI32, SwapBuffers) { gl_swap_buffers(); RET(1, 1); }
static uint16_t g_ramp[3 * 256];
static int g_ramp_init;
SHIM(GDI32, GetDeviceGammaRamp) {
    if (!g_ramp_init) { for (int i = 0; i < 256; i++) g_ramp[i] = g_ramp[256 + i] = g_ramp[512 + i] = (uint16_t)(i * 257); g_ramp_init = 1; }
    memcpy(gptr(ARG(1)), g_ramp, sizeof g_ramp);
    RET(1, 2);
}
SHIM(GDI32, SetDeviceGammaRamp) {
    memcpy(g_ramp, gptr(ARG(1)), sizeof g_ramp);
    g_ramp_init = 1;
    gl_set_gamma_ramp(g_ramp);
    RET(1, 2);
}
SHIM(GDI32, CreateFontA) { RET(0x40100, 14); }
SHIM(GDI32, CreateSolidBrush) { RET(0x40200, 1); }
SHIM(GDI32, DeleteDC) { RET(1, 1); }
SHIM(GDI32, SetBkColor) { RET(0, 2); }
SHIM(GDI32, SetTextColor) { RET(0, 2); }

// ------------------------------------------------------------------ ADVAPI32
SHIM(ADVAPI32, GetUserNameA) {
    const char *u = getlogin();
    if (!u) u = "player";
    uint32_t cap = g_rd32(ARG(1));
    uint32_t n = (uint32_t)strlen(u) + 1;
    if (n > cap) { g_wr32(ARG(1), n); rt_set_last_error(122); RET(0, 2); }
    memcpy(ARGP(0), u, n);
    g_wr32(ARG(1), n);
    RET(1, 2);
}

// ------------------------------------------------------------------ WINMM
SHIM(WINMM, timeGetTime) { RET(plat_time_ms(), 0); }
SHIM(WINMM, timeBeginPeriod) { RET(0, 1); }
SHIM(WINMM, timeEndPeriod) { RET(0, 1); }
SHIM(WINMM, joyGetNumDevs) { RET(0, 0); }
SHIM(WINMM, joyGetDevCapsA) { RET(165, 3); }      // JOYERR_UNPLUGGED
SHIM(WINMM, joyGetPosEx) { RET(165, 2); }
SHIM(WINMM, midiInGetNumDevs) { RET(0, 0); }
SHIM(WINMM, midiInGetDevCapsA) { RET(2, 3); }     // MMSYSERR_BADDEVICEID
SHIM(WINMM, midiInOpen) { RET(2, 5); }
SHIM(WINMM, midiInStart) { RET(0, 1); }
SHIM(WINMM, midiInClose) { RET(0, 1); }

// ------------------------------------------------------------------ SHELL32
SHIM(SHELL32, ShellExecuteA) {
    LOG("ShellExecuteA(%s, %s) ignored", ARG(1) ? ARGP(1) : "", ARG(2) ? ARGP(2) : "");
    RET(2, 6);   // <= 32: failure (no external browser/launcher from the game)
}

// ------------------------------------------------------------------ DDRAW: the engine only probes for video memory
SHIM(DDRAW, DirectDrawEnumerateExA) { RET(0, 3); }  // no callbacks: no extra devices
SHIM(DDRAW, DirectDrawCreateEx) {
    if (ARG(1)) g_wr32(ARG(1), 0);
    RET(0x80004005u, 4);   // E_FAIL: engine falls back to defaults
}
