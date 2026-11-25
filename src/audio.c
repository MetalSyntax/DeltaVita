#include "audio.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#define OUT_RATE 48000
#define GRAIN 512                  /* frames por bloque de salida */
#define RING (1 << 15)             /* frames en el búfer circular (~0,7 s) */

static int16_t g_ring[RING * 2];
static volatile uint32_t g_write, g_read;   /* un productor (núcleo) y un consumidor (hilo) */
static volatile double g_step = 1.0;        /* frames de entrada por frame de salida */
static volatile bool g_running, g_muted;
static SceUID g_thread = -1;
static int g_port = -1;

static uint32_t available(void) { return g_write - g_read; }

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    static int16_t out[GRAIN * 2];
    double pos = 0;
    while (g_running) {
        double step = g_step;
        uint32_t need = (uint32_t)(GRAIN * step) + 2;
        if (g_muted || available() < need) {
            memset(out, 0, sizeof(out)); /* sin datos: silencio en vez de repetir basura */
        } else {
            uint32_t base = g_read;
            for (int i = 0; i < GRAIN; i++) {
                uint32_t i0 = base + (uint32_t)pos;
                double frac = pos - (uint32_t)pos;
                for (int c = 0; c < 2; c++) {
                    int a = g_ring[((i0) & (RING - 1)) * 2 + c], b = g_ring[((i0 + 1) & (RING - 1)) * 2 + c];
                    out[i * 2 + c] = (int16_t)(a + (b - a) * frac);
                }
                pos += step;
            }
            g_read = base + (uint32_t)pos;
            pos -= (uint32_t)pos;
            /* si el núcleo va adelantado (más de ~0,2 s acumulado) se descarta lo viejo para no ir con retraso */
            if (available() > OUT_RATE / 5) g_read = g_write - OUT_RATE / 10;
        }
        sceAudioOutOutput(g_port, out);
    }
    return 0;
}

void vaudio_init(double core_rate) {
    vaudio_set_rate(core_rate);
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) return;
    g_running = true;
    g_thread = sceKernelCreateThread("audio", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (g_thread >= 0) sceKernelStartThread(g_thread, 0, NULL);
}

void vaudio_set_rate(double core_rate) {
    if (core_rate > 1000) g_step = core_rate / OUT_RATE;
}

void vaudio_push(const int16_t *frames, size_t count) {
    if (g_port < 0) return;
    uint32_t w = g_write;
    for (size_t i = 0; i < count; i++, w++) {
        g_ring[(w & (RING - 1)) * 2] = frames[i * 2];
        g_ring[(w & (RING - 1)) * 2 + 1] = frames[i * 2 + 1];
    }
    if (w - g_read > RING) g_read = w - RING; /* desborde: se pierde lo más viejo */
    g_write = w;
}

void vaudio_set_muted(bool muted) { g_muted = muted; }
void vaudio_clear(void) { g_read = g_write; }

void vaudio_quit(void) {
    g_running = false;
    if (g_thread >= 0) { sceKernelWaitThreadEnd(g_thread, NULL, NULL); sceKernelDeleteThread(g_thread); }
    if (g_port >= 0) sceAudioOutReleasePort(g_port);
    g_thread = -1; g_port = -1;
}
