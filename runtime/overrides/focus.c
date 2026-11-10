// Keep the engine's idea of "active app" in sync with the real window focus (CoDSP.exe and CoDMP.exe).
//
// The engine captures the mouse only while g_wv.activeApp is set, and only WM_ACTIVATE changes it. If an
// activation message is lost or arrives out of order (focus changes during window creation, fullscreen
// transitions, the console window's message loop...), the mouse stays free until the next real focus change, which
// is why alt-tabbing fixed it. Called from plat_pump: re-post WM_ACTIVATE whenever the two disagree.
#include "../rt.h"
#include "../platform.h"

#ifdef COD_MP
#define G_WV_HWND      0x1999d68u   // CoDMP.exe
#define G_WV_ACTIVEAPP 0x1994518u
#else
#define G_WV_HWND      0x15342c8u   // CoDSP.exe: set by the main window's WM_CREATE
#define G_WV_ACTIVEAPP 0x152ea6cu
#endif

void rt_focus_sync(int focused) {
    static uint32_t last;
    uint32_t now = plat_time_ms();
    if (now - last < 250) return;   // give a posted WM_ACTIVATE time to be dispatched
    last = now;
    if (!g_rd32(G_WV_HWND)) return;
    int active = g_rd32(G_WV_ACTIVEAPP) != 0;
    if (active == (focused != 0)) return;
    plat_post_msg(0, 0x001C, focused ? 1 : 0, 0);   // WM_ACTIVATEAPP
    plat_post_msg(0, 0x0006, focused ? 1 : 0, 0);   // WM_ACTIVATE (WA_ACTIVE / WA_INACTIVE)
}
