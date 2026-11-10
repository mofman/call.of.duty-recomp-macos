// Miles Sound System 6.5 (mss32.dll) replacement: software mixer over SDL audio.
//
// 2D samples and 3D samples play PCM8/PCM16/IMA-ADPCM data supplied by the engine (decoded to host float at start).
// Streams are read through the engine's AIL file callbacks (pk3 filesystem) on the calling (main) thread, then
// decoded with AudioToolbox (MP3/WAV/ADPCM) on a background thread; the mixer plays whatever is decoded so far.
// No guest code ever runs on the audio thread.
#include <AudioToolbox/AudioToolbox.h>
#include "../rt.h"
#include "../platform.h"
#include <pthread.h>
#include <math.h>
#include <string.h>

#define OUT_RATE 44100
#define MAX_VOICES 160

#define ARGF(n) ({ uint32_t _b = ARG(n); float _f; memcpy(&_f, &_b, 4); _f; })

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
enum { DIG_F_16 = 1, DIG_F_STEREO = 2, DIG_F_ADPCM = 4 };

typedef struct Pcm {          // decoded audio, interleaved float, `ch` channels at `rate`
    float *data;
    size_t frames, cap;       // frames decoded so far / allocated
    int ch, rate;
    volatile int complete;    // decoder finished (frames is final)
    size_t total_frames;      // estimate (streams) or exact
} Pcm;

typedef enum { V_2D, V_3D, V_STREAM } VKind;
typedef struct Voice {
    uint32_t handle;          // guest handle value
    VKind kind;
    int allocated;
    int status;
    Pcm pcm;
    double pos;               // frame position
    int rate;                 // playback rate (Hz) of the source
    int loops;                // remaining loop count (0 = infinite)
    float vol, pan;           // 0..1, 0..1 (0.5 centre)
    // sample-type state for 2D handles
    int fmt, sign8, adpcm_block;
    uint32_t addr, len;       // guest data
    // double-buffered streaming (load_sample_buffer)
    uint32_t buf_addr[2], buf_len[2];
    int buf_ready_mask;
    // 3D
    float x, y, z, min_d, max_d;
    // stream file data (encoded) + decoder thread
    uint8_t *file; size_t file_len;
    pthread_t dec; int dec_started; volatile int dec_stop;
    int paused;
} Voice;

static Voice g_v[MAX_VOICES];
static uint32_t g_driver, g_provider, g_last_err;
static uint32_t g_cb_open, g_cb_close, g_cb_seek, g_cb_read;
static float g_dist_factor = 1.0f;
static int g_started;

static int g_dbg = -1;
#define ADBG(...) do { if (g_dbg < 0) g_dbg = getenv("COD_AUDIODBG") != NULL; if (g_dbg) LOG("AUDIO " __VA_ARGS__); } while (0)
static void lock(void) { plat_audio_lock(); }
static void unlock(void) { plat_audio_unlock(); }

static Voice *voice_of(uint32_t h) {
    for (int i = 0; i < MAX_VOICES; i++) if (g_v[i].allocated && g_v[i].handle == h) return &g_v[i];
    return NULL;
}
static Voice *voice_new(VKind k) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!g_v[i].allocated) {
            Voice *v = &g_v[i];
            uint32_t h = v->handle ? v->handle : galloc(64);   // stable guest handle per slot
            memset(v, 0, sizeof *v);
            v->handle = h; v->kind = k; v->allocated = 1; v->status = SMP_DONE;
            v->vol = 1; v->pan = 0.5f; v->loops = 1; v->min_d = 1; v->max_d = 1e9f; v->fmt = DIG_F_16;
            return v;
        }
    }
    LOG("MSS: out of voices");
    return NULL;
}
static void pcm_free(Pcm *p) { free(p->data); memset(p, 0, sizeof *p); }

// ------------------------------------------------------------------ decoding of engine-supplied data
static const int ima_step[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,
    13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int ima_idx[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

static void pcm_reserve(Pcm *p, size_t frames) {
    if (frames > p->cap) { p->cap = frames + frames / 2 + 1024; p->data = realloc(p->data, p->cap * p->ch * sizeof(float)); }
}
// IMA ADPCM (MS layout): block = per-channel 4-byte header, then 4-byte words per channel (8 nibbles each)
static void decode_adpcm(Pcm *p, const uint8_t *d, size_t len, int ch, int block) {
    if (block <= 4 * ch) return;
    size_t per_block = (size_t)(block - 4 * ch) * 2 / ch + 1;
    size_t nblocks = (len + block - 1) / block;
    pcm_reserve(p, p->frames + nblocks * per_block);
    for (size_t b = 0; b < nblocks; b++) {
        const uint8_t *s = d + b * block;
        size_t avail = len - b * block < (size_t)block ? len - b * block : (size_t)block;
        if (avail < (size_t)4 * ch) break;
        int pred[2], idx[2];
        float *out = p->data + p->frames * ch;
        for (int c = 0; c < ch; c++) {
            pred[c] = (int16_t)(s[4 * c] | (s[4 * c + 1] << 8));
            idx[c] = s[4 * c + 2] > 88 ? 88 : s[4 * c + 2];
            out[c] = pred[c] / 32768.f;
        }
        size_t nf = 1;
        size_t words = (avail - 4 * ch) / (4 * ch);
        for (size_t w = 0; w < words; w++) {
            for (int c = 0; c < ch; c++) {
                const uint8_t *q = s + 4 * ch + (w * ch + c) * 4;
                for (int k = 0; k < 8; k++) {
                    int nib = (q[k >> 1] >> ((k & 1) * 4)) & 15;
                    int step = ima_step[idx[c]];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    if (nib & 8) diff = -diff;
                    pred[c] += diff;
                    if (pred[c] > 32767) pred[c] = 32767; else if (pred[c] < -32768) pred[c] = -32768;
                    idx[c] += ima_idx[nib];
                    if (idx[c] < 0) idx[c] = 0; else if (idx[c] > 88) idx[c] = 88;
                    out[(nf + w * 8 + k) * ch + c] = pred[c] / 32768.f;
                }
            }
        }
        nf += words * 8;
        p->frames += nf;
    }
}
static void decode_pcm(Pcm *p, const uint8_t *d, size_t len, int ch, int bits, int sign8) {
    size_t bpf = (size_t)ch * (bits / 8);
    size_t n = len / bpf;
    pcm_reserve(p, p->frames + n);
    float *out = p->data + p->frames * ch;
    for (size_t i = 0; i < n * ch; i++) {
        if (bits == 16) out[i] = (int16_t)(d[2 * i] | (d[2 * i + 1] << 8)) / 32768.f;
        else out[i] = sign8 ? (int8_t)d[i] / 128.f : (d[i] - 128) / 128.f;
    }
    p->frames += n;
}
// decode engine buffer according to a DIG_F_* format
static void decode_fmt(Pcm *p, const uint8_t *d, size_t len, int fmt, int sign8, int block, int rate) {
    int ch = (fmt & DIG_F_STEREO) ? 2 : 1;
    if (!p->ch) { p->ch = ch; p->rate = rate; }
    if (fmt & DIG_F_ADPCM) decode_adpcm(p, d, len, ch, block ? block : 512 * ch);
    else decode_pcm(p, d, len, ch, (fmt & DIG_F_16) ? 16 : 8, sign8);
    p->complete = 1;
    p->total_frames = p->frames;
}

// AILSOUNDINFO: format, data_ptr, data_len, rate, bits, channels, samples, block_size, initial_ptr (36 bytes)
static void decode_soundinfo(Pcm *p, uint32_t info) {
    uint32_t format = g_rd32(info), ptr = g_rd32(info + 4), len = g_rd32(info + 8), rate = g_rd32(info + 12);
    int bits = (int)g_rd32(info + 16), ch = (int)g_rd32(info + 20), block = (int)g_rd32(info + 28);
    if (ch < 1 || ch > 2) ch = 1;
    p->ch = ch; p->rate = (int)rate;
    if (format == 0x11) decode_adpcm(p, gptr(ptr), len, ch, block);
    else decode_pcm(p, gptr(ptr), len, ch, bits == 8 ? 8 : 16, 0);
    p->complete = 1;
    p->total_frames = p->frames;
}

// ------------------------------------------------------------------ mixer (audio thread)
static void mix_voice(Voice *v, float *acc, int frames) {
    Pcm *p = &v->pcm;
    if (!p->data || !p->ch || v->status != SMP_PLAYING || v->paused) return;
    double step = (double)(v->rate ? v->rate : p->rate) / OUT_RATE;
    float gl, gr;
    float vol = v->vol;
    if (v->kind == V_3D) {
        float d = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z) * g_dist_factor;
        float att = d <= v->min_d ? 1.0f : v->min_d / d;
        if (d > v->max_d) att = 0;
        vol *= att;
        float pan = d > 1e-3f ? v->x / (d / g_dist_factor) : 0;       // -1 left .. +1 right (listener-relative)
        float a = (pan + 1) * 0.25f * (float)M_PI;
        gl = cosf(a) * vol * 1.41421356f * 0.7071f; gr = sinf(a) * vol * 1.41421356f * 0.7071f;
    } else {
        float a = v->pan * 0.5f * (float)M_PI;
        gl = cosf(a) * vol * 1.41421356f; gr = sinf(a) * vol * 1.41421356f;
        if (gl > vol) gl = vol; if (gr > vol) gr = vol;
    }
    for (int i = 0; i < frames; i++) {
        size_t f = (size_t)v->pos;
        if (f >= p->frames) {
            if (!p->complete) break;                      // stream decoder behind: wait
            if (v->loops != 1) {
                if (v->loops > 1) v->loops--;
                v->pos -= (double)p->frames;
                if (v->pos < 0) v->pos = 0;
                f = (size_t)v->pos;
                if (!p->frames) { v->status = SMP_DONE; return; }
            } else { v->status = SMP_DONE; return; }
        }
        float l, r;
        if (p->ch == 2) { l = p->data[2 * f]; r = p->data[2 * f + 1]; }
        else l = r = p->data[f];
        if (v->kind == V_3D && p->ch == 2) { float m = 0.5f * (l + r); l = r = m; }
        acc[2 * i] += l * gl;
        acc[2 * i + 1] += r * gr;
        v->pos += step;
    }
}
static void mixer(int16_t *out, int frames) {
    static float acc[2 * 4096];
    memset(acc, 0, sizeof(float) * 2 * frames);
    for (int i = 0; i < MAX_VOICES; i++) if (g_v[i].allocated) mix_voice(&g_v[i], acc, frames);
    for (int i = 0; i < 2 * frames; i++) {
        float s = acc[i];
        s = s > 1 ? 1 : s < -1 ? -1 : s;
        out[i] = (int16_t)(s * 32767.f);
    }
}

// ------------------------------------------------------------------ startup / driver
SHIM(MSS32, _AIL_startup_0) { RET(1, 0); }
SHIM(MSS32, _AIL_shutdown_0) { RET(0, 0); }
SHIM(MSS32, _AIL_set_preference_8) { RET(0, 2); }
SHIM(MSS32, _AIL_set_redist_directory_4) { RET(ARG(0), 1); }
SHIM(MSS32, _AIL_last_error_0) {
    if (!g_last_err) g_last_err = gstrdup("");
    RET(g_last_err, 0);
}
SHIM(MSS32, _AIL_set_DirectSound_HWND_8) { RET(0, 2); }
SHIM(MSS32, _AIL_open_digital_driver_16) {
    LOG("MSS: open_digital_driver(%u Hz, %u bits, %u ch)", ARG(0), ARG(1), ARG(2));
    if (!g_driver) g_driver = galloc(256);
    if (!g_started) { plat_audio_start(mixer, OUT_RATE); g_started = 1; }
    RET(g_driver, 4);
}
SHIM(MSS32, _AIL_digital_CPU_percent_4) { RET(0, 1); }
SHIM(MSS32, _AIL_set_digital_master_room_type_8) { RET(0, 2); }
SHIM(MSS32, _AIL_set_file_callbacks_16) {
    g_cb_open = ARG(0); g_cb_close = ARG(1); g_cb_seek = ARG(2); g_cb_read = ARG(3);
    RET(0, 4);
}
SHIM(MSS32, _AIL_minimum_sample_buffer_size_12) { RET(8192, 3); }

// ------------------------------------------------------------------ 2D samples
SHIM(MSS32, _AIL_allocate_sample_handle_4) {
    lock();
    Voice *v = voice_new(V_2D);
    unlock();
    RET(v ? v->handle : 0, 1);
}
SHIM(MSS32, _AIL_release_sample_handle_4) {
    lock();
    Voice *v = voice_of(ARG(0));
    if (v) { pcm_free(&v->pcm); v->allocated = 0; }
    unlock();
    RET(0, 1);
}
SHIM(MSS32, _AIL_init_sample_4) {
    lock();
    Voice *v = voice_of(ARG(0));
    if (v) {
        pcm_free(&v->pcm);
        v->status = SMP_DONE; v->pos = 0; v->rate = 0; v->loops = 1; v->vol = 1; v->pan = 0.5f;
        v->fmt = DIG_F_16; v->sign8 = 0; v->addr = v->len = 0; v->buf_ready_mask = 3; v->paused = 0;
    }
    unlock();
    RET(0, 1);
}
SHIM(MSS32, _AIL_set_sample_type_12) {
    Voice *v = voice_of(ARG(0));
    if (v) { v->fmt = (int)ARG(1); v->sign8 = ARG(2) & 1; }
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_sample_adpcm_block_size_8) { Voice *v = voice_of(ARG(0)); if (v) v->adpcm_block = (int)ARG(1); RET(0, 2); }
SHIM(MSS32, _AIL_set_sample_address_12) {
    Voice *v = voice_of(ARG(0));
    if (v) { v->addr = ARG(1); v->len = ARG(2); }
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_sample_playback_rate_8) { Voice *v = voice_of(ARG(0)); if (v) { v->rate = (int)ARG(1); if (!v->pcm.rate) v->pcm.rate = v->rate; } RET(0, 2); }
SHIM(MSS32, _AIL_sample_playback_rate_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)(v->rate ? v->rate : v->pcm.rate) : 0, 1); }
SHIM(MSS32, _AIL_set_sample_volume_pan_12) {
    Voice *v = voice_of(ARG(0));
    if (v) { v->vol = ARGF(1); v->pan = ARGF(2); }
    RET(0, 3);
}
SHIM(MSS32, _AIL_sample_volume_pan_12) {
    Voice *v = voice_of(ARG(0));
    float vol = v ? v->vol : 0, pan = v ? v->pan : 0.5f;
    if (ARG(1)) memcpy(gptr(ARG(1)), &vol, 4);
    if (ARG(2)) memcpy(gptr(ARG(2)), &pan, 4);
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_sample_loop_count_8) { Voice *v = voice_of(ARG(0)); if (v) v->loops = (int)ARG(1); RET(0, 2); }
SHIM(MSS32, _AIL_set_sample_reverb_levels_12) { RET(0, 3); }
SHIM(MSS32, _AIL_start_sample_4) {
    lock();
    Voice *v = voice_of(ARG(0));
    if (v) ADBG("start_sample %08x fmt=%d rate=%d len=%u vol=%.2f pan=%.2f loops=%d", v->handle, v->fmt, v->rate, v->len, v->vol, v->pan, v->loops);
    if (v) {
        if (v->addr && v->len) {
            int rate = v->rate ? v->rate : 22050;
            pcm_free(&v->pcm);
            decode_fmt(&v->pcm, gptr(v->addr), v->len, v->fmt, v->sign8, v->adpcm_block, rate);
        }
        v->pos = 0; v->status = SMP_PLAYING; v->paused = 0;
    }
    unlock();
    RET(0, 1);
}
SHIM(MSS32, _AIL_stop_sample_4) { lock(); Voice *v = voice_of(ARG(0)); if (v && v->status == SMP_PLAYING) v->status = SMP_STOPPED; unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_resume_sample_4) { lock(); Voice *v = voice_of(ARG(0)); if (v && v->status == SMP_STOPPED) v->status = SMP_PLAYING; unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_end_sample_4) { lock(); Voice *v = voice_of(ARG(0)); if (v) { v->status = SMP_DONE; v->pos = 0; } unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_sample_status_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)v->status : SMP_FREE, 1); }
static uint32_t ms_of(const Pcm *p, double frames) { return p->rate ? (uint32_t)(frames * 1000.0 / p->rate) : 0; }
SHIM(MSS32, _AIL_set_sample_ms_position_8) {
    Voice *v = voice_of(ARG(0));
    if (v && v->pcm.rate) v->pos = (double)ARG(1) * v->pcm.rate / 1000.0;
    RET(0, 2);
}
SHIM(MSS32, _AIL_sample_ms_position_12) {
    Voice *v = voice_of(ARG(0));
    if (ARG(1)) g_wr32(ARG(1), v ? ms_of(&v->pcm, (double)v->pcm.total_frames) : 0);
    if (ARG(2)) g_wr32(ARG(2), v ? ms_of(&v->pcm, v->pos) : 0);
    RET(0, 3);
}
SHIM(MSS32, _AIL_sample_position_4) {
    Voice *v = voice_of(ARG(0));
    if (!v) RET(0, 1);
    int bpf = ((v->fmt & DIG_F_STEREO) ? 2 : 1) * ((v->fmt & DIG_F_16) ? 2 : 1);
    RET((uint32_t)(v->pos * bpf), 1);
}
// double-buffered streaming into a sample handle
SHIM(MSS32, _AIL_sample_buffer_ready_4) {
    Voice *v = voice_of(ARG(0));
    if (!v) RET(0xFFFFFFFFu, 1);
    lock();
    // a buffer is "ready" (needs data) once the decoded backlog is short
    double left = (double)v->pcm.frames - v->pos;
    int r = -1;
    if (v->status != SMP_PLAYING || left < (v->pcm.rate ? v->pcm.rate / 4 : 4096)) r = (v->buf_ready_mask & 1) ? 0 : 1;
    unlock();
    RET((uint32_t)r, 1);
}
SHIM(MSS32, _AIL_load_sample_buffer_16) {   // (S, buffer_num, ptr, len)
    lock();
    Voice *v = voice_of(ARG(0));
    if (v && ARG(2) && ARG(3)) {
        int rate = v->rate ? v->rate : 22050;
        Pcm *p = &v->pcm;
        // drop consumed frames to keep the buffer bounded
        size_t used = (size_t)v->pos;
        if (p->data && used > 0 && used <= p->frames) {
            memmove(p->data, p->data + used * p->ch, (p->frames - used) * p->ch * sizeof(float));
            p->frames -= used; v->pos -= used;
        }
        int ch = (v->fmt & DIG_F_STEREO) ? 2 : 1;
        if (!p->ch) { p->ch = ch; p->rate = rate; }
        if (v->fmt & DIG_F_ADPCM) decode_adpcm(p, gptr(ARG(2)), ARG(3), ch, v->adpcm_block ? v->adpcm_block : 512 * ch);
        else decode_pcm(p, gptr(ARG(2)), ARG(3), ch, (v->fmt & DIG_F_16) ? 16 : 8, v->sign8);
        p->complete = 0;
        p->total_frames = p->frames;
        v->buf_ready_mask ^= 1;
        if (v->status != SMP_PLAYING) { v->status = SMP_PLAYING; }
    } else if (v && ARG(3) == 0) {
        v->pcm.complete = 1;   // zero-length buffer: end of data
    }
    unlock();
    RET(0, 4);
}

// WAV parsing (AIL_WAV_info)
SHIM(MSS32, _AIL_WAV_info_8) {
    uint32_t d = ARG(0), info = ARG(1);
    const uint8_t *p = gptr(d);
    if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) RET(0, 2);
    uint32_t total = g_rd32(d + 4) + 8, o = 12;
    uint32_t fmt = 1, ch = 1, rate = 22050, bits = 16, block = 0, data = 0, dlen = 0;
    while (o + 8 <= total) {
        uint32_t id = g_rd32(d + o), sz = g_rd32(d + o + 4);
        if (id == 0x20746d66) {   // "fmt "
            fmt = g_rd16(d + o + 8); ch = g_rd16(d + o + 10); rate = g_rd32(d + o + 12);
            block = g_rd16(d + o + 20); bits = g_rd16(d + o + 22);
        } else if (id == 0x61746164) { data = d + o + 8; dlen = sz; break; }   // "data"
        o += 8 + sz + (sz & 1);
    }
    if (!data) RET(0, 2);
    uint32_t samples = 0;
    if (fmt == 0x11 && block > 4 * ch) {
        uint32_t per = (block - 4 * ch) * 2 / ch + 1;
        samples = (dlen / block) * per;
    } else if (bits && ch) samples = dlen / (bits / 8 * ch);
    g_wr32(info, fmt); g_wr32(info + 4, data); g_wr32(info + 8, dlen); g_wr32(info + 12, rate);
    g_wr32(info + 16, fmt == 0x11 ? 4 : bits); g_wr32(info + 20, ch); g_wr32(info + 24, samples);
    g_wr32(info + 28, block); g_wr32(info + 32, data);
    RET(1, 2);
}
// AILMIXINFO starts with AILSOUNDINFO. Output: PCM in dest_format at dest_rate (linear resample).
static uint32_t process_audio(uint32_t dest, uint32_t dest_size, int dest_rate, int dest_fmt, uint32_t src, int count_only) {
    Pcm p = {0};
    decode_soundinfo(&p, src);
    int och = (dest_fmt & DIG_F_STEREO) ? 2 : 1, obits = (dest_fmt & DIG_F_16) ? 16 : 8;
    if (dest_rate <= 0) dest_rate = p.rate;
    size_t oframes = p.rate ? (size_t)((double)p.frames * dest_rate / p.rate) : 0;
    size_t bytes = oframes * och * (obits / 8);
    if (count_only) { pcm_free(&p); return (uint32_t)bytes; }
    if (bytes > dest_size) { oframes = dest_size / (och * (obits / 8)); bytes = oframes * och * (obits / 8); }
    uint8_t *o = gptr(dest);
    for (size_t i = 0; i < oframes; i++) {
        double sp = (double)i * p.rate / dest_rate;
        size_t f = (size_t)sp; double fr = sp - f;
        size_t f2 = f + 1 < p.frames ? f + 1 : f;
        for (int c = 0; c < och; c++) {
            int sc = p.ch == 2 ? c : 0;
            float s;
            if (och == 1 && p.ch == 2) s = 0.5f * (p.data[2 * f] + p.data[2 * f + 1]) * (float)(1 - fr) + 0.5f * (p.data[2 * f2] + p.data[2 * f2 + 1]) * (float)fr;
            else s = p.data[f * p.ch + sc] * (float)(1 - fr) + p.data[f2 * p.ch + sc] * (float)fr;
            s = s > 1 ? 1 : s < -1 ? -1 : s;
            if (obits == 16) { int16_t v = (int16_t)(s * 32767); memcpy(o, &v, 2); o += 2; }
            else *o++ = (uint8_t)(s * 127 + 128);
        }
    }
    pcm_free(&p);
    return (uint32_t)bytes;
}
SHIM(MSS32, _AIL_process_digital_audio_24) {   // (dest, dest_size, dest_rate, dest_format, num_srcs, AILMIXINFO*)
    RET(process_audio(ARG(0), ARG(1), (int)ARG(2), (int)ARG(3), ARG(5), 0), 6);
}
SHIM(MSS32, _AIL_size_processed_digital_audio_16) {   // (dest_rate, dest_format, num_srcs, AILMIXINFO*)
    RET(process_audio(0, 0, (int)ARG(0), (int)ARG(1), ARG(3), 1), 4);
}

// ------------------------------------------------------------------ 3D provider + samples
SHIM(MSS32, _AIL_enumerate_3D_providers_12) {   // (HPROENUM *next, HPROVIDER *dest, char **name)
    static uint32_t name;
    if (!name) name = gstrdup("Miles Fast 2D Positional Audio");
    if (g_rd32(ARG(0)) != 0) RET(0, 3);
    if (!g_provider) g_provider = galloc(64);
    g_wr32(ARG(0), 1);
    g_wr32(ARG(1), g_provider);
    g_wr32(ARG(2), name);
    RET(1, 3);
}
SHIM(MSS32, _AIL_open_3D_provider_4) { RET(0, 1); }    // M3D_NOERR
SHIM(MSS32, _AIL_close_3D_provider_4) { RET(0, 1); }
SHIM(MSS32, _AIL_set_3D_provider_preference_12) { RET(0, 3); }
SHIM(MSS32, _AIL_3D_provider_attribute_12) {   // (provider, name, void *value)
    const char *n = ARG(1) ? ARGP(1) : "";
    uint32_t v = 0;
    if (strcasestr(n, "maximum supported samples")) v = 64;
    LOG("MSS: 3D provider attribute '%s' -> %u", n, v);
    if (ARG(2)) g_wr32(ARG(2), v);
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_3D_room_type_8) { RET(0, 2); }
SHIM(MSS32, _AIL_set_3D_distance_factor_8) { g_dist_factor = ARGF(1) > 0 ? ARGF(1) : 1; RET(0, 2); }
SHIM(MSS32, _AIL_allocate_3D_sample_handle_4) {
    lock();
    Voice *v = voice_new(V_3D);
    unlock();
    RET(v ? v->handle : 0, 1);
}
SHIM(MSS32, _AIL_set_3D_sample_info_8) {   // (S, AILSOUNDINFO*) -> 1 ok
    ADBG("set_3D_info fmt=%u len=%u rate=%u bits=%u ch=%u block=%u", g_rd32(ARG(1)), g_rd32(ARG(1) + 8), g_rd32(ARG(1) + 12), g_rd32(ARG(1) + 16), g_rd32(ARG(1) + 20), g_rd32(ARG(1) + 28));
    lock();
    Voice *v = voice_of(ARG(0));
    if (v) {
        pcm_free(&v->pcm);
        decode_soundinfo(&v->pcm, ARG(1));
        v->rate = v->pcm.rate; v->pos = 0; v->status = SMP_DONE; v->loops = 1; v->paused = 0;
    }
    unlock();
    RET(v ? 1 : 0, 2);
}
SHIM(MSS32, _AIL_set_3D_sample_offset_8) {   // byte offset in the source data -> frame
    Voice *v = voice_of(ARG(0));
    if (v && v->pcm.ch) v->pos = (double)ARG(1) / (2 * v->pcm.ch);
    RET(0, 2);
}
SHIM(MSS32, _AIL_3D_sample_offset_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)(v->pos * 2 * (v->pcm.ch ? v->pcm.ch : 1)) : 0, 1); }
SHIM(MSS32, _AIL_3D_sample_length_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)(v->pcm.total_frames * 2 * (v->pcm.ch ? v->pcm.ch : 1)) : 0, 1); }
SHIM(MSS32, _AIL_set_3D_sample_playback_rate_8) { Voice *v = voice_of(ARG(0)); if (v) v->rate = (int)ARG(1); RET(0, 2); }
SHIM(MSS32, _AIL_3D_sample_playback_rate_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)v->rate : 0, 1); }
SHIM(MSS32, _AIL_set_3D_sample_volume_8) { Voice *v = voice_of(ARG(0)); if (v) v->vol = ARGF(1); RET(0, 2); }
SHIM(MSS32, _AIL_3D_sample_volume_4) {
    Voice *v = voice_of(ARG(0));
    float f = v ? v->vol : 0;
    uint32_t b; memcpy(&b, &f, 4);
    // float return: x87 ST(0)
    FPUSH((double)f);
    (void)b;
    RET(0, 1);
}
SHIM(MSS32, _AIL_set_3D_sample_loop_count_8) { Voice *v = voice_of(ARG(0)); if (v) v->loops = (int)ARG(1); RET(0, 2); }
SHIM(MSS32, _AIL_set_3D_sample_distances_12) {   // (S, max, min)
    Voice *v = voice_of(ARG(0));
    if (v) { v->max_d = ARGF(1); v->min_d = ARGF(2) > 0 ? ARGF(2) : 1; }
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_3D_sample_effects_level_8) { RET(0, 2); }
SHIM(MSS32, _AIL_set_3D_sample_preference_12) { RET(0, 3); }
SHIM(MSS32, _AIL_set_3D_position_16) {
    Voice *v = voice_of(ARG(0));
    if (v) { v->x = ARGF(1); v->y = ARGF(2); v->z = ARGF(3); }
    RET(0, 4);
}
SHIM(MSS32, _AIL_3D_position_16) {
    Voice *v = voice_of(ARG(0));
    float p[3] = {v ? v->x : 0, v ? v->y : 0, v ? v->z : 0};
    for (int i = 0; i < 3; i++) if (ARG(1 + i)) memcpy(gptr(ARG(1 + i)), &p[i], 4);
    RET(0, 4);
}
SHIM(MSS32, _AIL_start_3D_sample_4) {
    lock();
    Voice *v = voice_of(ARG(0));
    if (v) ADBG("start_3D %08x frames=%zu ch=%d rate=%d vol=%.2f pos=(%.1f %.1f %.1f) min=%.1f max=%.1f loops=%d df=%.3f", v->handle, v->pcm.frames, v->pcm.ch, v->rate, v->vol, v->x, v->y, v->z, v->min_d, v->max_d, v->loops, g_dist_factor);
    if (v) { v->status = SMP_PLAYING; v->paused = 0; }
    unlock();
    RET(0, 1);
}
SHIM(MSS32, _AIL_stop_3D_sample_4) { lock(); Voice *v = voice_of(ARG(0)); if (v && v->status == SMP_PLAYING) v->status = SMP_STOPPED; unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_resume_3D_sample_4) { lock(); Voice *v = voice_of(ARG(0)); if (v && v->status == SMP_STOPPED) v->status = SMP_PLAYING; unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_end_3D_sample_4) { lock(); Voice *v = voice_of(ARG(0)); if (v) { v->status = SMP_DONE; v->pos = 0; } unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_3D_sample_status_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)v->status : SMP_FREE, 1); }

// ------------------------------------------------------------------ streams
typedef struct { const uint8_t *d; size_t n; } MemFile;
static OSStatus mf_read(void *ud, SInt64 pos, UInt32 req, void *buf, UInt32 *got) {
    MemFile *m = ud;
    if (pos >= (SInt64)m->n) { *got = 0; return noErr; }
    size_t k = m->n - (size_t)pos < req ? m->n - (size_t)pos : req;
    memcpy(buf, m->d + pos, k);
    *got = (UInt32)k;
    return noErr;
}
static SInt64 mf_size(void *ud) { return (SInt64)((MemFile *)ud)->n; }

static void *stream_decoder(void *arg) {
    Voice *v = arg;
    MemFile mf = {v->file, v->file_len};
    AudioFileID af = NULL;
    const char *ext = "";
    AudioFileTypeID hint = (v->file_len > 4 && !memcmp(v->file, "RIFF", 4)) ? kAudioFileWAVEType : kAudioFileMP3Type;
    (void)ext;
    if (AudioFileOpenWithCallbacks(&mf, mf_read, NULL, mf_size, NULL, hint, &af) != noErr) {
        LOG("MSS stream: AudioToolbox cannot open stream data (%zu bytes)", v->file_len);
        v->pcm.complete = 1;
        return NULL;
    }
    ExtAudioFileRef ef = NULL;
    ExtAudioFileWrapAudioFileID(af, false, &ef);
    AudioStreamBasicDescription in = {0};
    UInt32 sz = sizeof in;
    ExtAudioFileGetProperty(ef, kExtAudioFileProperty_FileDataFormat, &sz, &in);
    int ch = in.mChannelsPerFrame >= 2 ? 2 : 1;
    AudioStreamBasicDescription cf = {0};
    cf.mSampleRate = in.mSampleRate; cf.mFormatID = kAudioFormatLinearPCM;
    cf.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cf.mChannelsPerFrame = (UInt32)ch; cf.mBitsPerChannel = 32; cf.mFramesPerPacket = 1;
    cf.mBytesPerFrame = cf.mBytesPerPacket = 4 * (UInt32)ch;
    ExtAudioFileSetProperty(ef, kExtAudioFileProperty_ClientDataFormat, sizeof cf, &cf);
    SInt64 total = 0; sz = sizeof total;
    ExtAudioFileGetProperty(ef, kExtAudioFileProperty_FileLengthFrames, &sz, &total);
    lock();
    v->pcm.ch = ch; v->pcm.rate = (int)in.mSampleRate; v->pcm.total_frames = total > 0 ? (size_t)total : 0;
    pcm_reserve(&v->pcm, total > 0 ? (size_t)total + 4096 : 1 << 20);
    unlock();
    float tmp[8192 * 2];
    while (!v->dec_stop) {
        AudioBufferList bl;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = (UInt32)ch;
        bl.mBuffers[0].mDataByteSize = sizeof tmp;
        bl.mBuffers[0].mData = tmp;
        UInt32 frames = 8192;
        if (ExtAudioFileRead(ef, &frames, &bl) != noErr || frames == 0) break;
        lock();
        pcm_reserve(&v->pcm, v->pcm.frames + frames);
        memcpy(v->pcm.data + v->pcm.frames * ch, tmp, (size_t)frames * ch * sizeof(float));
        v->pcm.frames += frames;
        unlock();
    }
    ExtAudioFileDispose(ef);
    AudioFileClose(af);
    lock();
    v->pcm.total_frames = v->pcm.frames;
    v->pcm.complete = 1;
    unlock();
    return NULL;
}

// read a whole file through the engine's AIL file callbacks (stdcall guest functions)
static uint8_t *read_via_callbacks(Cpu *cpu, const char *name, size_t *len) {
    uint32_t hslot = galloc(4), gname = gstrdup(name);
    uint32_t ok = call_guest(cpu, g_cb_open, 0, 2, gname, hslot);
    gfree(gname);
    if (!ok) { gfree(hslot); return NULL; }
    uint32_t fh = g_rd32(hslot);
    gfree(hslot);
    int32_t size = (int32_t)call_guest(cpu, g_cb_seek, 0, 3, fh, 0, 2);
    call_guest(cpu, g_cb_seek, 0, 3, fh, 0, 0);
    if (size <= 0) { call_guest(cpu, g_cb_close, 0, 1, fh); return NULL; }
    uint8_t *buf = malloc((size_t)size);
    uint32_t chunk = galloc(65536);
    size_t got = 0;
    while (got < (size_t)size) {
        uint32_t want = (size_t)size - got > 65536 ? 65536 : (uint32_t)((size_t)size - got);
        uint32_t n = call_guest(cpu, g_cb_read, 0, 3, fh, chunk, want);
        if (!n) break;
        memcpy(buf + got, gptr(chunk), n);
        got += n;
    }
    gfree(chunk);
    call_guest(cpu, g_cb_close, 0, 1, fh);
    *len = got;
    return buf;
}

SHIM(MSS32, _AIL_open_stream_12) {   // (dig, filename, stream_mem)
    const char *name = ARGP(1);
    size_t len = 0;
    uint8_t *data = NULL;
    if (g_cb_open) data = read_via_callbacks(cpu, name, &len);
    if (!data) {
        char host[PATH_MAX];
        rt_map_path(name, host, sizeof host);
        FILE *f = fopen(host, "rb");
        if (f) { fseek(f, 0, SEEK_END); len = (size_t)ftell(f); fseek(f, 0, SEEK_SET); data = malloc(len); len = fread(data, 1, len, f); fclose(f); }
    }
    if (!data) { LOG("MSS: open_stream(%s) failed", name); RET(0, 3); }
    lock();
    Voice *v = voice_new(V_STREAM);
    unlock();
    if (!v) { free(data); RET(0, 3); }
    v->file = data; v->file_len = len; v->status = SMP_STOPPED;
    v->dec_started = pthread_create(&v->dec, NULL, stream_decoder, v) == 0;
    LOG("MSS: open_stream(%s) %zu bytes -> %08x", name, len, v->handle);
    RET(v->handle, 3);
}
SHIM(MSS32, _AIL_close_stream_4) {
    Voice *v = voice_of(ARG(0));
    if (v) {
        v->dec_stop = 1;
        if (v->dec_started) pthread_join(v->dec, NULL);
        lock();
        pcm_free(&v->pcm);
        free(v->file); v->file = NULL;
        v->allocated = 0;
        unlock();
    }
    RET(0, 1);
}
SHIM(MSS32, _AIL_start_stream_4) { lock(); Voice *v = voice_of(ARG(0)); if (v) { v->pos = 0; v->status = SMP_PLAYING; v->paused = 0; } unlock(); RET(0, 1); }
SHIM(MSS32, _AIL_pause_stream_8) { Voice *v = voice_of(ARG(0)); if (v) v->paused = ARG(1) != 0; RET(0, 2); }
SHIM(MSS32, _AIL_set_stream_loop_count_8) { Voice *v = voice_of(ARG(0)); if (v) v->loops = (int)ARG(1); RET(0, 2); }
SHIM(MSS32, _AIL_set_stream_volume_pan_12) { Voice *v = voice_of(ARG(0)); if (v) { v->vol = ARGF(1); v->pan = ARGF(2); } RET(0, 3); }
SHIM(MSS32, _AIL_stream_volume_pan_12) {
    Voice *v = voice_of(ARG(0));
    float vol = v ? v->vol : 0, pan = v ? v->pan : 0.5f;
    if (ARG(1)) memcpy(gptr(ARG(1)), &vol, 4);
    if (ARG(2)) memcpy(gptr(ARG(2)), &pan, 4);
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_stream_playback_rate_8) { Voice *v = voice_of(ARG(0)); if (v) v->rate = (int)ARG(1); RET(0, 2); }
SHIM(MSS32, _AIL_stream_playback_rate_4) { Voice *v = voice_of(ARG(0)); RET(v ? (uint32_t)(v->rate ? v->rate : v->pcm.rate) : 0, 1); }
SHIM(MSS32, _AIL_set_stream_reverb_levels_12) { RET(0, 3); }
SHIM(MSS32, _AIL_stream_status_4) {
    Voice *v = voice_of(ARG(0));
    if (!v) RET(0xFFFFFFFFu, 1);
    RET((uint32_t)(v->paused && v->status == SMP_PLAYING ? SMP_STOPPED : v->status), 1);
}
SHIM(MSS32, _AIL_stream_ms_position_12) {
    Voice *v = voice_of(ARG(0));
    if (ARG(1)) g_wr32(ARG(1), v ? ms_of(&v->pcm, (double)v->pcm.total_frames) : 0);
    if (ARG(2)) g_wr32(ARG(2), v ? ms_of(&v->pcm, v->pos) : 0);
    RET(0, 3);
}
SHIM(MSS32, _AIL_set_stream_ms_position_8) {
    Voice *v = voice_of(ARG(0));
    if (v && v->pcm.rate) { lock(); v->pos = (double)ARG(1) * v->pcm.rate / 1000.0; unlock(); }
    RET(0, 2);
}
