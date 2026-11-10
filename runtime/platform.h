// SDL3-backed platform layer.
#pragma once
#include <stdint.h>

typedef struct Msg { uint32_t hwnd, msg, wp, lp, time, x, y; } Msg;

void plat_set_mode(int w, int h);
void *plat_metal_layer(void);                 // CAMetalLayer* of the game window (creates it on first use)
void plat_drawable_size(int *w, int *h);
void plat_set_fullscreen(int fs);
int  plat_take_dump_request(void);
void plat_dump_rgba(const uint8_t *pix, int w, int h, int flip_y, const char *path);
void plat_pump(void);
int  plat_peek_msg(Msg *m, uint32_t fmin, uint32_t fmax, int remove);
void plat_post_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t plat_time_ms(void);
void plat_set_title(const char *s);
void plat_show_cursor(int show);
void plat_warp_mouse(int x, int y);
void plat_mac_init(void);                     // runtime/mac.m
void plat_mac_activate(void);
void rt_focus_sync(int focused);              // runtime/overrides/focus.c
int  plat_mode_w(void);
int  plat_mode_h(void);

// input state (game coordinates)
extern uint8_t  g_vk_down[256];        // 1 if down
extern uint8_t  g_vk_toggle[256];
extern uint8_t  g_vk_pressed_since[256]; // GetAsyncKeyState low bit
extern int      g_mouse_x, g_mouse_y;
extern int32_t  g_mouse_dx, g_mouse_dy, g_mouse_dz;  // accumulated deltas (DirectInput)
extern uint8_t  g_mouse_btn[4];
extern uint8_t  g_dik_down[256];          // DirectInput keyboard state (set-1 scancodes, 0x80|ext)
extern uint32_t g_main_hwnd;
extern int      g_quit_requested;

// audio: mixer callback produces interleaved stereo s16 frames at rate
void plat_audio_start(void (*mix)(int16_t *out, int frames), int rate);
void plat_audio_lock(void);
void plat_audio_unlock(void);

// DirectInput buffered mouse events
typedef struct { uint32_t ofs, data, ts, seq; } DIEvt;
int  plat_mouse_event_pop(DIEvt *e);
