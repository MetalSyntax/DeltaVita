/**
 * @file runner.c
 * @brief Ejecutor de un núcleo libretro (se enlaza una vez por núcleo -> app0:cores/<núcleo>.bin).
 *
 * El lanzador escribe ux0:data/DeltaVita/launch.txt (ruta de la ROM y título) y hace LoadExec de este
 * binario; al salir se vuelve al lanzador con otro LoadExec.
 */
#include <math.h>
#include <stdarg.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include "audio.h"
#include "common.h"
#include "libretro.h"
#include "skin.h"
#include "ui.h"

/* weak: desmume2015 define el suyo (128 MB) */
__attribute__((weak)) int _newlib_heap_size_user = 200 * 1024 * 1024;

/** gpsp (recompilador dinámico) pide al frontend un bloque de memoria ejecutable, igual que en RetroArch. */
int getVMBlock(void) {
    static SceUID block = -1;
    if (block < 0) block = sceKernelAllocMemBlockForVM("dv_code", 16 * 1024 * 1024);
    return block;
}

/* ---------- estado ---------- */

static int g_sys;
static char g_rom[512], g_title[160], g_name[160];   /* g_name = archivo de la ROM */
static struct retro_system_av_info g_av;
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static vita2d_texture *g_frame;
static unsigned g_fw, g_fh;
static void *g_rom_data;
static Skin g_skin;
static bool g_skin_on;

static uint32_t g_pad;           /* bits RETRO_DEVICE_ID_JOYPAD_* que ve el núcleo */
static int16_t g_ax[2][2];       /* stick izquierdo/derecho -> x, y (-0x7fff..0x7fff) */
static int16_t g_ptr_x, g_ptr_y; /* pantalla táctil de la DS */
static bool g_ptr_down;
static float g_stick_draw_x, g_stick_draw_y;

static bool g_ff, g_muted, g_sustain;
static uint32_t g_held;          /* botones retenidos con "Mantener botones" */
static uint32_t g_prev_raw;
static float g_gx, g_gy, g_gw, g_gh; /* dónde se dibujó el juego (para el puntero de la DS) */

static char g_toast[128];
static int g_toast_frames;

static void toast(const char *msg) { snprintf(g_toast, sizeof(g_toast), "%s", msg); g_toast_frames = 120; }

/* ---------- rutas por juego ---------- */

static void path_srm(char *o, size_t n) { snprintf(o, n, DATA_DIR "/saves/%s.srm", g_name); }
static void path_states(char *o, size_t n) { snprintf(o, n, DATA_DIR "/states/%s", g_name); }
static void path_state(char *o, size_t n, const char *slot, const char *ext) {
    snprintf(o, n, DATA_DIR "/states/%s/%s.%s", g_name, slot, ext);
}
static void path_cheats(char *o, size_t n) { snprintf(o, n, DATA_DIR "/cheats/%s.txt", g_name); }

/* ---------- callbacks de libretro ---------- */

static const char *core_option(const char *key) {
    bool land = !g_config.portrait;
    if (!strcmp(key, "desmume_screens_layout")) return land && g_skin_on ? "left/right" : "top/bottom";
    if (!strcmp(key, "desmume_pointer_type")) return "touch";
    if (!strcmp(key, "desmume_pointer_mouse")) return "enabled";
    if (!strcmp(key, "gpsp_drc")) return "enabled";
    if (!strcmp(key, "desmume_cpu_mode")) return "interpreter"; /* el JIT ARM de desmume no reserva memoria ejecutable en Vita */
    return NULL;
}

static bool g_vars_changed;

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logf_("[core] %s", buf);
}

static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = DATA_DIR "/system"; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = DATA_DIR "/saves"; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f > RETRO_PIXEL_FORMAT_RGB565) return false;
        g_fmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = core_option(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = g_vars_changed; g_vars_changed = false; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: g_av = *(struct retro_system_av_info *)data; vaudio_set_rate(g_av.timing.sample_rate); return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: g_av.geometry = *(struct retro_game_geometry *)data; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned *)data = lang_es() ? RETRO_LANGUAGE_SPANISH : RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    default:
        return false; /* incluye SET_HW_RENDER: aquí todo es por software */
    }
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return; /* fotograma repetido */
    SceGxmTextureFormat fmt = g_fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB
                            : g_fmt == RETRO_PIXEL_FORMAT_RGB565 ? SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB
                                                                 : SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1RGB;
    if (!g_frame || vita2d_texture_get_width(g_frame) < w || vita2d_texture_get_height(g_frame) < h ||
        vita2d_texture_get_format(g_frame) != fmt) {
        if (g_frame) { vita2d_wait_rendering_done(); vita2d_free_texture(g_frame); }
        unsigned tw = w > g_av.geometry.max_width ? w : g_av.geometry.max_width;
        unsigned th = h > g_av.geometry.max_height ? h : g_av.geometry.max_height;
        g_frame = vita2d_create_empty_texture_format(tw, th, fmt);
        if (!g_frame) return;
    }
    size_t bpp = g_fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    size_t stride = vita2d_texture_get_stride(g_frame);
    uint8_t *dst = vita2d_texture_get_datap(g_frame);
    const uint8_t *src = data;
    for (unsigned y = 0; y < h; y++) memcpy(dst + y * stride, src + y * pitch, w * bpp);
    g_fw = w; g_fh = h;
}

static bool g_ff_extra; /* fotogramas extra del avance rápido: su audio se descarta */

static void audio_sample(int16_t l, int16_t r) {
    if (g_ff_extra) return;
    int16_t f[2] = {l, r};
    vaudio_push(f, 1);
}

static size_t audio_batch(const int16_t *data, size_t frames) {
    if (!g_ff_extra) vaudio_push(data, frames);
    return frames;
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)g_pad;
        return (g_pad >> id) & 1;
    case RETRO_DEVICE_ANALOG:
        return index < 2 && id < 2 ? g_ax[index][id] : 0;
    case RETRO_DEVICE_POINTER:
        if (id == RETRO_DEVICE_ID_POINTER_X) return g_ptr_x;
        if (id == RETRO_DEVICE_ID_POINTER_Y) return g_ptr_y;
        if (id == RETRO_DEVICE_ID_POINTER_PRESSED) return g_ptr_down;
        return 0;
    }
    return 0;
}

/* ---------- entrada ---------- */

/** Nombre de input de la skin -> bit del RetroPad (mismo mapeo que DeltaDroid). */
static int retro_id(const char *n) {
    static const struct { const char *n; int id; } common[] = {
        {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {"start", RETRO_DEVICE_ID_JOYPAD_START}, {"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
    };
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) if (!strcmp(n, common[i].n)) return common[i].id;
    if (g_sys == SYS_N64) {
        if (!strcmp(n, "a")) return RETRO_DEVICE_ID_JOYPAD_B;
        if (!strcmp(n, "b")) return RETRO_DEVICE_ID_JOYPAD_Y;
        if (!strcmp(n, "z")) return RETRO_DEVICE_ID_JOYPAD_L2;
    } else if (g_sys == SYS_GEN) {
        if (!strcmp(n, "a")) return RETRO_DEVICE_ID_JOYPAD_Y;
        if (!strcmp(n, "b")) return RETRO_DEVICE_ID_JOYPAD_B;
        if (!strcmp(n, "c")) return RETRO_DEVICE_ID_JOYPAD_A;
        if (!strcmp(n, "x")) return RETRO_DEVICE_ID_JOYPAD_L;
        if (!strcmp(n, "y")) return RETRO_DEVICE_ID_JOYPAD_X;
        if (!strcmp(n, "z")) return RETRO_DEVICE_ID_JOYPAD_R;
        return -1;
    }
    if (!strcmp(n, "a")) return RETRO_DEVICE_ID_JOYPAD_A;
    if (!strcmp(n, "b")) return RETRO_DEVICE_ID_JOYPAD_B;
    if (!strcmp(n, "x")) return RETRO_DEVICE_ID_JOYPAD_X;
    if (!strcmp(n, "y")) return RETRO_DEVICE_ID_JOYPAD_Y;
    if (!strcmp(n, "l")) return RETRO_DEVICE_ID_JOYPAD_L;
    if (!strcmp(n, "r")) return RETRO_DEVICE_ID_JOYPAD_R;
    return -1;
}

#define BIT(id) (1u << (id))

/** Botones de la Vita -> RetroPad por posición (✕ abajo = B, ○ derecha = A, □ = Y, △ = X). */
static uint32_t physical_to_pad(uint32_t b) {
    bool rot = g_config.portrait; /* Vita girada: la cruceta gira con la imagen */
    uint32_t up = rot ? SCE_CTRL_LEFT : SCE_CTRL_UP, right = rot ? SCE_CTRL_UP : SCE_CTRL_RIGHT;
    uint32_t down = rot ? SCE_CTRL_RIGHT : SCE_CTRL_DOWN, left = rot ? SCE_CTRL_DOWN : SCE_CTRL_LEFT;
    uint32_t p = 0;
    if (b & up) p |= BIT(RETRO_DEVICE_ID_JOYPAD_UP);
    if (b & down) p |= BIT(RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (b & left) p |= BIT(RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (b & right) p |= BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (b & SCE_CTRL_CROSS) p |= BIT(RETRO_DEVICE_ID_JOYPAD_B);
    if (b & SCE_CTRL_CIRCLE) p |= BIT(RETRO_DEVICE_ID_JOYPAD_A);
    if (b & SCE_CTRL_SQUARE) p |= BIT(RETRO_DEVICE_ID_JOYPAD_Y);
    if (b & SCE_CTRL_TRIANGLE) p |= BIT(RETRO_DEVICE_ID_JOYPAD_X);
    if (b & SCE_CTRL_START) p |= BIT(RETRO_DEVICE_ID_JOYPAD_START);
    if (b & SCE_CTRL_SELECT) p |= BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (b & SCE_CTRL_LTRIGGER) p |= BIT(g_sys == SYS_N64 ? RETRO_DEVICE_ID_JOYPAD_L2 : RETRO_DEVICE_ID_JOYPAD_L);
    if (b & SCE_CTRL_RTRIGGER) p |= BIT(RETRO_DEVICE_ID_JOYPAD_R);
    return p;
}

static int16_t axis(unsigned char v) {
    int d = (int)v - 128;
    if (abs(d) < 20) return 0;
    return (int16_t)(d * 0x7fff / 127);
}

static void open_menu(void);

/** Lee mando y pantalla táctil; devuelve los botones recién pulsados (para menús). */
static uint32_t read_input(SceCtrlData *pad) {
    sceCtrlPeekBufferPositive(0, pad, 1);
    uint32_t pressed = pad->buttons & ~g_prev_raw;
    g_prev_raw = pad->buttons;
    return pressed;
}

static void game_input(const SceCtrlData *pad, uint32_t pressed) {
    if ((pad->buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START) &&
        (pressed & (SCE_CTRL_SELECT | SCE_CTRL_START))) {
        open_menu();
        return;
    }
    uint32_t p = physical_to_pad(pad->buttons);
    float lx = axis(pad->lx), ly = axis(pad->ly), rx = axis(pad->rx), ry = axis(pad->ry);
    if (g_config.portrait) { float t = lx; lx = -ly; ly = t; t = rx; rx = -ry; ry = t; }
    int16_t sx = (int16_t)lx, sy = (int16_t)ly;
    if (g_sys != SYS_N64) { /* el stick izquierdo hace de cruceta */
        if (sx < -0x4000) p |= BIT(RETRO_DEVICE_ID_JOYPAD_LEFT);
        if (sx > 0x4000) p |= BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT);
        if (sy < -0x4000) p |= BIT(RETRO_DEVICE_ID_JOYPAD_UP);
        if (sy > 0x4000) p |= BIT(RETRO_DEVICE_ID_JOYPAD_DOWN);
    }
    int16_t ax = sx, ay = sy, cx = (int16_t)rx, cy = (int16_t)ry;
    g_ptr_down = false;
    g_stick_draw_x = g_stick_draw_y = 0;

    /* pantalla táctil: botones de la skin (o el táctil de la DS sin skin) */
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    static bool menu_touch_prev;
    bool menu_touch = false;
    for (int i = 0; i < touch.reportNum; i++) {
        float x, y;
        ui_touch_to_logical(touch.report[i].x / 2.0f, touch.report[i].y / 2.0f, &x, &y);
        int hit = g_skin_on ? skin_hit(&g_skin, x, y) : -1;
        const SkinItem *m = hit >= 0 ? &g_skin.items[hit] : NULL;
        bool on_game = x >= g_gx && y >= g_gy && x < g_gx + g_gw && y < g_gy + g_gh;
        if ((m && m->kind == ITEM_TOUCHSCREEN) || (!m && g_sys == SYS_DS && on_game)) {
            g_ptr_down = true;
            g_ptr_x = (int16_t)(((x - g_gx) / g_gw * 2 - 1) * 0x7fff);
            g_ptr_y = (int16_t)(((y - g_gy) / g_gh * 2 - 1) * 0x7fff);
            continue;
        }
        if (!m) continue;
        float dx = (x - (m->x + m->w / 2)) / (m->w / 2), dy = (y - (m->y + m->h / 2)) / (m->h / 2);
        dx = fmaxf(-1, fminf(1, dx)); dy = fmaxf(-1, fminf(1, dy));
        if (m->kind == ITEM_STICK) {
            ax = (int16_t)(dx * 0x7fff); ay = (int16_t)(dy * 0x7fff);
            g_stick_draw_x = dx; g_stick_draw_y = dy;
        } else if (m->kind == ITEM_DPAD) {
            const char *dirs[4] = {dy < -0.35f ? m->names[0] : NULL, dy > 0.35f ? m->names[1] : NULL,
                                   dx < -0.35f ? m->names[2] : NULL, dx > 0.35f ? m->names[3] : NULL};
            for (int d = 0; d < 4; d++) if (dirs[d]) { int id = retro_id(dirs[d]); if (id >= 0) p |= BIT(id); }
        } else {
            for (int k = 0; k < m->count; k++) {
                const char *n = m->names[k];
                if (!strcmp(n, "menu")) { menu_touch = true; continue; }
                if (n[0] == 'c' && n[1] >= 'A' && n[1] <= 'Z') { /* cUp/cDown/cLeft/cRight -> stick derecho */
                    if (!strcmp(n, "cUp")) cy = -0x7fff;
                    else if (!strcmp(n, "cDown")) cy = 0x7fff;
                    else if (!strcmp(n, "cLeft")) cx = -0x7fff;
                    else cx = 0x7fff;
                    continue;
                }
                int id = retro_id(n);
                if (id >= 0) p |= BIT(id);
            }
        }
    }
    if (menu_touch && !menu_touch_prev) { menu_touch_prev = true; open_menu(); return; }
    menu_touch_prev = menu_touch;

    /* "Mantener botones": cada pulsación nueva alterna el botón entre retenido y suelto */
    static uint32_t prev_p;
    if (g_sustain) {
        uint32_t fresh = p & ~prev_p;
        g_held ^= fresh;
        prev_p = p;
        p = g_held;
    } else {
        prev_p = p;
    }
    g_pad = p;
    g_ax[0][0] = ax; g_ax[0][1] = ay; g_ax[1][0] = cx; g_ax[1][1] = cy;
}

/* ---------- imagen ---------- */

static void game_rect(float *x, float *y, float *w, float *h) {
    if (g_skin_on) { *x = g_skin.gx; *y = g_skin.gy; *w = g_skin.gw; *h = g_skin.gh; }
    else { *x = 0; *y = 0; *w = ui_w(); *h = ui_h(); }
}

static void draw_game(void) {
    if (!g_frame || !g_fw) return;
    float bx, by, bw, bh;
    game_rect(&bx, &by, &bw, &bh);
    float aspect = g_av.geometry.aspect_ratio > 0 ? g_av.geometry.aspect_ratio : (float)g_fw / g_fh;
    if (g_sys == SYS_DS) aspect = (float)g_fw / g_fh; /* las dos pantallas juntas */
    float w, h;
    if (!g_skin_on && g_config.scale_mode == 2) { w = bw; h = bh; }
    else {
        w = bw; h = w / aspect;
        if (h > bh) { h = bh; w = h * aspect; }
        if (!g_skin_on && g_config.scale_mode == 1) { /* escala entera: píxeles nítidos */
            float k = floorf(h / g_fh);
            if (k >= 1) { h = g_fh * k; w = h * aspect; }
        }
    }
    float x = bx + (bw - w) / 2, y = by + (bh - h) / 2;
    g_gx = x; g_gy = y; g_gw = w; g_gh = h;
    SceGxmTextureFilter f = g_config.smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
    vita2d_texture_set_filters(g_frame, f, f);
    vita2d_draw_texture_part_scale(g_frame, x, y, 0, 0, g_fw, g_fh, w / g_fw, h / g_fh);
}

static void reload_skin(void) {
    skin_free(&g_skin);
    g_skin_on = false;
    if (!g_config.use_skin[g_sys]) return;
    int vw = g_config.portrait ? SCREEN_H : SCREEN_W, vh = g_config.portrait ? SCREEN_W : SCREEN_H;
    char path[256];
    const char *chosen = g_config.skin[g_sys];
    snprintf(path, sizeof(path), DATA_DIR "/skins/%s/%s", SYSTEMS[g_sys].id, chosen);
    if (!chosen[0] || !file_exists(path)) snprintf(path, sizeof(path), "app0:skins/%s.deltaskin", SYSTEMS[g_sys].id);
    g_skin_on = skin_load(&g_skin, path, vw, vh);
    if (!g_skin_on) toast(T("No se pudo cargar la skin: juego sin skin", "Could not load the skin: playing without it"));
}

/* ---------- PNG (capturas y miniaturas) ---------- */

static bool save_png(const char *path, int scale_down) {
    if (!g_frame || !g_fw) return false;
    unsigned w = g_fw / scale_down, h = g_fh / scale_down;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    uint8_t *row = malloc(w * 3);
    if (!png || !info || !row || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info); free(row); fclose(f); return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    const uint8_t *data = vita2d_texture_get_datap(g_frame);
    size_t stride = vita2d_texture_get_stride(g_frame);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *src = data + y * scale_down * stride;
        for (unsigned x = 0; x < w; x++) {
            unsigned sx = x * scale_down;
            uint8_t r, g, b;
            if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t v = ((const uint32_t *)src)[sx];
                r = v >> 16; g = v >> 8; b = v;
            } else {
                uint16_t v = ((const uint16_t *)src)[sx];
                if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) { r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; }
                else { r = ((v >> 10) & 31) << 3; g = ((v >> 5) & 31) << 3; b = (v & 31) << 3; }
            }
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(row);
    fclose(f);
    return true;
}

/* ---------- partidas y estados ---------- */

static void save_sram(void) {
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *d = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!n || !d) return;
    char p[512];
    path_srm(p, sizeof(p));
    write_file(p, d, n);
}

static void load_sram(void) {
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), got = 0;
    void *d = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    char p[512];
    path_srm(p, sizeof(p));
    void *buf = n && d ? read_file(p, &got) : NULL;
    if (buf) memcpy(d, buf, got < n ? got : n);
    free(buf);
}

static bool save_state(const char *slot) {
    size_t n = retro_serialize_size();
    void *buf = n ? malloc(n) : NULL;
    bool ok = buf && retro_serialize(buf, n);
    char p[512];
    path_states(p, sizeof(p));
    mkdirs(p);
    if (ok) { path_state(p, sizeof(p), slot, "state"); ok = write_file(p, buf, n); }
    if (ok) { path_state(p, sizeof(p), slot, "png"); save_png(p, g_fw > 320 ? 2 : 1); }
    free(buf);
    return ok;
}

static bool load_state(const char *slot) {
    char p[512];
    path_state(p, sizeof(p), slot, "state");
    size_t n = 0;
    void *buf = read_file(p, &n);
    bool ok = buf && retro_unserialize(buf, n);
    free(buf);
    if (ok) vaudio_clear();
    return ok;
}

/* ---------- trucos ---------- */

typedef struct { bool on; char name[64]; char code[256]; } Cheat;
static Cheat g_cheats[64];
static int g_ncheats;

static void cheats_apply(void) {
    retro_cheat_reset();
    for (int i = 0; i < g_ncheats; i++) retro_cheat_set(i, g_cheats[i].on, g_cheats[i].code);
}

static void cheats_load(void) {
    char p[512];
    path_cheats(p, sizeof(p));
    FILE *f = fopen(p, "r");
    g_ncheats = 0;
    if (!f) return;
    char line[400];
    while (g_ncheats < 64 && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL;
        if (!b) continue;
        *a = *b = 0;
        Cheat *c = &g_cheats[g_ncheats++];
        c->on = line[0] == '1';
        snprintf(c->name, sizeof(c->name), "%s", a + 1);
        snprintf(c->code, sizeof(c->code), "%s", b + 1);
    }
    fclose(f);
}

static void cheats_save(void) {
    char p[512];
    mkdirs(DATA_DIR "/cheats");
    path_cheats(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < g_ncheats; i++) fprintf(f, "%d|%s|%s\n", g_cheats[i].on, g_cheats[i].name, g_cheats[i].code);
    fclose(f);
}

/** Teclado del sistema (IME). false si se canceló. */
static bool ime_input(const char *title, char *out, size_t n) {
    static uint16_t title16[64], text16[256], init16[2];
    for (size_t i = 0; i < 63 && title[i]; i++) title16[i] = (uint8_t)title[i], title16[i + 1] = 0;
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.title = title16;
    p.maxTextLength = (SceUInt32)(n - 1 < 255 ? n - 1 : 255);
    p.initialText = init16;
    p.inputTextBuffer = text16;
    if (sceImeDialogInit(&p) < 0) return false;
    bool ok = false;
    for (;;) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult r = {0};
            sceImeDialogGetResult(&r);
            ok = r.button == SCE_IME_DIALOG_BUTTON_ENTER;
            break;
        }
    }
    sceImeDialogTerm();
    if (ok) { /* UTF-16 -> UTF-8 (BMP) */
        size_t o = 0;
        for (int i = 0; text16[i] && o + 4 < n; i++) {
            uint16_t c = text16[i];
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800) { out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 63); }
            else { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
        }
        out[o] = 0;
    }
    return ok;
}

/* ---------- menú de pausa ---------- */

typedef enum { M_SAVE, M_LOAD, M_FF, M_RESET, M_SHOT, M_CHEATS, M_HOLD, M_MUTE, M_SKIN, M_ROTATE, M_EXIT, M_COUNT } MenuItem;
typedef enum { SCREEN_NONE, SCREEN_MENU, SCREEN_SAVE, SCREEN_LOAD, SCREEN_CHEATS } Screen;

static Screen g_screen;
static int g_sel, g_slot_sel, g_cheat_sel;
static bool g_quit;

#define SLOTS 8
static const char *slot_name(int i) {
    static char buf[16];
    if (i == SLOTS) return "auto";
    snprintf(buf, sizeof(buf), "slot%d", i + 1);
    return buf;
}
static vita2d_texture *g_thumbs[SLOTS + 1];
static bool g_has[SLOTS + 1], g_locked[SLOTS + 1];
static char g_dates[SLOTS + 1][32];

static void slots_refresh(void) {
    vita2d_wait_rendering_done();
    for (int i = 0; i <= SLOTS; i++) {
        if (g_thumbs[i]) { vita2d_free_texture(g_thumbs[i]); g_thumbs[i] = NULL; }
        char p[512];
        path_state(p, sizeof(p), slot_name(i), "state");
        SceIoStat st;
        g_has[i] = sceIoGetstat(p, &st) >= 0;
        g_dates[i][0] = 0;
        if (g_has[i]) snprintf(g_dates[i], sizeof(g_dates[i]), "%02d/%02d %02d:%02d",
                               st.st_mtime.day, st.st_mtime.month, st.st_mtime.hour, st.st_mtime.minute);
        path_state(p, sizeof(p), slot_name(i), "lock");
        g_locked[i] = file_exists(p);
        path_state(p, sizeof(p), slot_name(i), "png");
        if (g_has[i] && file_exists(p)) g_thumbs[i] = vita2d_load_PNG_file(p);
    }
}

static void open_menu(void) {
    save_sram();
    vaudio_set_muted(true);
    g_screen = SCREEN_MENU;
}

static void close_menu(void) {
    g_screen = SCREEN_NONE;
    vaudio_clear();
    vaudio_set_muted(g_muted);
}

static const char *menu_label(MenuItem m) {
    switch (m) {
    case M_SAVE: return T("Guardar estado", "Save State");
    case M_LOAD: return T("Cargar estado", "Load State");
    case M_FF: return T("Avance rápido", "Fast Forward");
    case M_RESET: return T("Reiniciar", "Reset");
    case M_SHOT: return T("Captura", "Screenshot");
    case M_CHEATS: return T("Trucos", "Cheat Codes");
    case M_HOLD: return T("Mantener botones", "Hold Buttons");
    case M_MUTE: return T("Silenciar", "Mute");
    case M_SKIN: return T("Usar skin", "Use Skin");
    case M_ROTATE: return T("Girar pantalla", "Rotate Screen");
    default: return T("Salir", "Quit");
    }
}

static bool menu_toggled(MenuItem m) {
    return (m == M_FF && g_ff) || (m == M_HOLD && g_sustain) || (m == M_MUTE && g_muted) || (m == M_SKIN && g_config.use_skin[g_sys]);
}

static void menu_activate(MenuItem m) {
    switch (m) {
    case M_SAVE: slots_refresh(); g_slot_sel = 0; g_screen = SCREEN_SAVE; return;
    case M_LOAD: slots_refresh(); g_slot_sel = 0; g_screen = SCREEN_LOAD; return;
    case M_CHEATS: g_cheat_sel = 0; g_screen = SCREEN_CHEATS; return;
    case M_FF: g_ff = !g_ff; break;
    case M_RESET: retro_reset(); break;
    case M_SHOT: {
        char p[512], name[64];
        mkdirs(DATA_DIR "/screenshots");
        clean_title(g_name, name, sizeof(name));
        snprintf(p, sizeof(p), DATA_DIR "/screenshots/%s_%lu.png", name, (unsigned long)time(NULL));
        toast(save_png(p, 1) ? T("Captura guardada en ux0:data/DeltaVita/screenshots", "Screenshot saved")
                             : T("No se pudo guardar la captura", "Could not save the screenshot"));
        break;
    }
    case M_HOLD:
        g_sustain = !g_sustain;
        g_held = 0;
        if (g_sustain) toast(T("Pulsa los botones a mantener; otra vez para soltarlos", "Press buttons to hold; again to release"));
        break;
    case M_MUTE: g_muted = !g_muted; break;
    case M_SKIN:
        g_config.use_skin[g_sys] = !g_config.use_skin[g_sys];
        config_save(); reload_skin(); g_vars_changed = true;
        break;
    case M_ROTATE:
        g_config.portrait = !g_config.portrait;
        config_save(); reload_skin(); g_vars_changed = true;
        break;
    case M_EXIT: g_quit = true; return;
    default: break;
    }
    close_menu();
}

static void draw_tile(float x, float y, float w, float h, const char *label, bool sel, bool on) {
    uint32_t accent = SYSTEMS[g_sys].color;
    ui_round_rect(x, y, w, h, 14, on ? accent : COL_SURFACE);
    if (sel) {
        ui_round_rect(x - 3, y - 3, w + 6, 3, 2, COL_TEXT);
        ui_round_rect(x - 3, y + h, w + 6, 3, 2, COL_TEXT);
        ui_rect(x - 3, y, 3, h, COL_TEXT);
        ui_rect(x + w, y, 3, h, COL_TEXT);
    }
    ui_text_center(x + w / 2, y + h / 2 - 10, COL_TEXT, 18, label);
}

static void draw_menu(void) {
    int W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, COL_SCRIM);
    int cols = W > H ? 4 : 2;
    float pad = 14, tw = (W - 48 - pad * (cols - 1)) / cols, th = W > H ? 76 : 70;
    int rows = (M_COUNT + cols - 1) / cols;
    float panel_h = 96 + rows * (th + pad) + 40;
    float py = H - panel_h;
    ui_round_rect(0, py, W, panel_h + 20, 24, RGBA8(0x1C, 0x1C, 0x21, 0xF6));
    ui_text_fit(24, py + 18, COL_TEXT, 26, W - 48, g_title);
    ui_text(24, py + 52, SYSTEMS[g_sys].color, 18, SYSTEMS[g_sys].label);
    for (int i = 0; i < M_COUNT; i++) {
        float x = 24 + (i % cols) * (tw + pad), y = py + 90 + (i / cols) * (th + pad);
        draw_tile(x, y, tw, th, menu_label(i), i == g_sel, menu_toggled(i));
    }
    ui_hints(RGBA8(0x1C, 0x1C, 0x21, 0xFF), GLYPH_CROSS, T("Elegir", "Select"), GLYPH_CIRCLE, T("Reanudar", "Resume"), -1);
}

static void draw_slots(bool saving) {
    int W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, RGBA8(0x12, 0x12, 0x16, 0xF8));
    ui_text(24, 18, COL_TEXT, 26, saving ? menu_label(M_SAVE) : menu_label(M_LOAD));
    int n = saving ? SLOTS : SLOTS + 1, cols = W > H ? 3 : 2;
    float pad = 14, tw = (W - 48 - pad * (cols - 1)) / cols, th = W > H ? 132 : 150;
    int first_row = g_slot_sel / cols > 1 ? g_slot_sel / cols - 1 : 0; /* desplazamiento simple */
    for (int i = 0; i < n; i++) {
        int row = i / cols - first_row;
        if (row < 0 || 64 + row * (th + pad) + th > H - 44) continue;
        float x = 24 + (i % cols) * (tw + pad), y = 64 + row * (th + pad);
        bool sel = i == g_slot_sel;
        ui_round_rect(x, y, tw, th, 12, sel ? ui_mix(COL_SURFACE, SYSTEMS[g_sys].color, 0.45f) : COL_SURFACE);
        float iw = th * 1.15f;
        ui_round_rect(x + 8, y + 8, iw, th - 16, 8, RGBA8(0, 0, 0, 0xFF));
        if (g_thumbs[i]) ui_texture_fit(g_thumbs[i], x + 8, y + 8, iw, th - 16, true, 0xFFFFFFFF);
        char label[32];
        if (i == SLOTS) snprintf(label, sizeof(label), "%s", T("Automático", "Auto Save"));
        else snprintf(label, sizeof(label), "%s %d", T("Ranura", "Slot"), i + 1);
        ui_text(x + iw + 18, y + 14, COL_TEXT, 20, label);
        ui_text(x + iw + 18, y + 42, COL_DIM, 16, g_has[i] ? g_dates[i] : T("Vacía", "Empty"));
        if (g_locked[i]) ui_text(x + iw + 18, y + 66, SYSTEMS[g_sys].color, 16, T("Bloqueada", "Locked"));
    }
    ui_hints(RGBA8(0x1C, 0x1C, 0x21, 0xFF), GLYPH_CROSS, saving ? T("Guardar", "Save") : T("Cargar", "Load"),
             GLYPH_TRIANGLE, T("Bloquear", "Lock"), GLYPH_SQUARE, T("Borrar", "Delete"), GLYPH_CIRCLE, T("Atrás", "Back"), -1);
}

static void slots_input(uint32_t pressed, bool saving) {
    int n = saving ? SLOTS : SLOTS + 1, cols = ui_w() > ui_h() ? 3 : 2;
    if (pressed & SCE_CTRL_RIGHT) g_slot_sel = (g_slot_sel + 1) % n;
    if (pressed & SCE_CTRL_LEFT) g_slot_sel = (g_slot_sel + n - 1) % n;
    if (pressed & SCE_CTRL_DOWN && g_slot_sel + cols < n) g_slot_sel += cols;
    if (pressed & SCE_CTRL_UP && g_slot_sel - cols >= 0) g_slot_sel -= cols;
    if (pressed & SCE_CTRL_CIRCLE) { g_screen = SCREEN_MENU; return; }
    char p[512];
    const char *slot = slot_name(g_slot_sel);
    if (pressed & SCE_CTRL_TRIANGLE && g_has[g_slot_sel]) {
        path_state(p, sizeof(p), slot, "lock");
        if (g_locked[g_slot_sel]) sceIoRemove(p); else write_file(p, "1", 1);
        slots_refresh();
    }
    if (pressed & SCE_CTRL_SQUARE && g_has[g_slot_sel] && !g_locked[g_slot_sel]) {
        const char *ext[] = {"state", "png"};
        for (int i = 0; i < 2; i++) { path_state(p, sizeof(p), slot, ext[i]); sceIoRemove(p); }
        slots_refresh();
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (saving) {
            if (g_locked[g_slot_sel]) { toast(T("Ranura bloqueada", "Slot is locked")); return; }
            toast(save_state(slot) ? T("Estado guardado", "State saved") : T("No se pudo guardar", "Could not save"));
        } else {
            if (!g_has[g_slot_sel]) return;
            toast(load_state(slot) ? T("Estado cargado", "State loaded") : T("No se pudo cargar", "Could not load"));
        }
        close_menu();
    }
}

static void draw_cheats(void) {
    int W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, RGBA8(0x12, 0x12, 0x16, 0xF8));
    ui_text(24, 18, COL_TEXT, 26, menu_label(M_CHEATS));
    ui_text_fit(24, 52, COL_DIM, 16, W - 48, T("Game Genie, Action Replay o GameShark, según la consola", "Game Genie, Action Replay or GameShark codes"));
    if (!g_ncheats) ui_text(24, 100, COL_DIM, 20, T("Todavía no hay trucos. Pulsa □ para agregar uno.", "No cheats yet. Press □ to add one."));
    for (int i = 0; i < g_ncheats; i++) {
        float y = 90 + i * 52;
        if (y > H - 96) break;
        bool sel = i == g_cheat_sel;
        ui_round_rect(24, y, W - 48, 46, 10, sel ? ui_mix(COL_SURFACE, SYSTEMS[g_sys].color, 0.45f) : COL_SURFACE);
        ui_text_fit(40, y + 6, COL_TEXT, 18, W - 200, g_cheats[i].name);
        ui_text_fit(40, y + 26, COL_DIM, 14, W - 200, g_cheats[i].code);
        ui_round_rect(W - 110, y + 12, 60, 22, 11, g_cheats[i].on ? SYSTEMS[g_sys].color : RGBA8(0x50, 0x50, 0x58, 0xFF));
        ui_text_center(W - 80, y + 14, COL_TEXT, 14, g_cheats[i].on ? T("SÍ", "ON") : "NO");
    }
    ui_hints(RGBA8(0x1C, 0x1C, 0x21, 0xFF), GLYPH_CROSS, T("Activar", "Toggle"), GLYPH_SQUARE, T("Agregar", "Add"),
             GLYPH_TRIANGLE, T("Borrar", "Delete"), GLYPH_CIRCLE, T("Atrás", "Back"), -1);
}

static void cheats_input(uint32_t pressed) {
    if (pressed & SCE_CTRL_DOWN && g_cheat_sel + 1 < g_ncheats) g_cheat_sel++;
    if (pressed & SCE_CTRL_UP && g_cheat_sel > 0) g_cheat_sel--;
    if (pressed & SCE_CTRL_CIRCLE) { g_screen = SCREEN_MENU; return; }
    if (pressed & SCE_CTRL_CROSS && g_ncheats) { g_cheats[g_cheat_sel].on = !g_cheats[g_cheat_sel].on; cheats_save(); cheats_apply(); }
    if (pressed & SCE_CTRL_TRIANGLE && g_ncheats) {
        memmove(&g_cheats[g_cheat_sel], &g_cheats[g_cheat_sel + 1], (g_ncheats - g_cheat_sel - 1) * sizeof(Cheat));
        g_ncheats--;
        if (g_cheat_sel >= g_ncheats && g_cheat_sel) g_cheat_sel--;
        cheats_save(); cheats_apply();
    }
    if (pressed & SCE_CTRL_SQUARE && g_ncheats < 64) {
        Cheat c = {true, "", ""};
        if (!ime_input(T("Código del truco", "Cheat code"), c.code, sizeof(c.code)) || !c.code[0]) return;
        for (char *s = c.code; *s; s++) if (*s == ' ' || *s == '\n') *s = '+'; /* varias líneas -> "+" de libretro */
        if (!ime_input(T("Nombre (opcional)", "Name (optional)"), c.name, sizeof(c.name)) || !c.name[0])
            snprintf(c.name, sizeof(c.name), "%s %d", T("Truco", "Cheat"), g_ncheats + 1);
        g_cheats[g_ncheats++] = c;
        g_cheat_sel = g_ncheats - 1;
        cheats_save(); cheats_apply();
    }
}

static void menu_input(uint32_t pressed) {
    int cols = ui_w() > ui_h() ? 4 : 2;
    if (pressed & SCE_CTRL_RIGHT) g_sel = (g_sel + 1) % M_COUNT;
    if (pressed & SCE_CTRL_LEFT) g_sel = (g_sel + M_COUNT - 1) % M_COUNT;
    if (pressed & SCE_CTRL_DOWN && g_sel + cols < M_COUNT) g_sel += cols;
    if (pressed & SCE_CTRL_UP && g_sel - cols >= 0) g_sel -= cols;
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) { close_menu(); return; }
    if (pressed & SCE_CTRL_CROSS) menu_activate((MenuItem)g_sel);
}

/* ---------- arranque ---------- */

static bool read_launch(void) {
    FILE *f = fopen(DATA_DIR "/launch.txt", "r");
    if (!f) return false;
    bool ok = fgets(g_rom, sizeof(g_rom), f) != NULL;
    if (!fgets(g_title, sizeof(g_title), f)) g_title[0] = 0;
    fclose(f);
    g_rom[strcspn(g_rom, "\r\n")] = 0;
    g_title[strcspn(g_title, "\r\n")] = 0;
    return ok && g_rom[0];
}

static void back_to_launcher(void) {
    sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    sceKernelExitProcess(0);
}

static void fatal(const char *msg) {
    logf_("ERROR: %s\n", msg);
    for (int i = 0; i < 240; i++) {
        SceCtrlData pad;
        uint32_t pressed = read_input(&pad);
        ui_begin(false);
        ui_text_center(SCREEN_W / 2, 230, COL_TEXT, 22, msg);
        ui_hints(COL_SURFACE, GLYPH_CIRCLE, T("Volver", "Back"), -1);
        ui_end();
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) break;
    }
    back_to_launcher();
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    log_init("runner");
    config_load();
    ui_init();
    if (!read_launch()) fatal(T("No se indicó ningún juego", "No game was selected"));
    snprintf(g_name, sizeof(g_name), "%s", base_name(g_rom));
    g_sys = system_for_file(g_name);
    if (g_sys < 0) fatal(T("Formato de juego no soportado", "Unsupported game format"));
    if (!g_title[0]) clean_title(g_name, g_title, sizeof(g_title));
    logf_("juego %s (%s) núcleo %s\n", g_rom, SYSTEMS[g_sys].label, SYSTEMS[g_sys].core);
    mkdirs(DATA_DIR "/saves");
    mkdirs(DATA_DIR "/system");

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_system_info si = {0};
    retro_get_system_info(&si);
    struct retro_game_info gi = {.path = g_rom};
    if (!si.need_fullpath) {
        size_t n = 0;
        g_rom_data = read_file(g_rom, &n);
        if (!g_rom_data) fatal(T("No se pudo leer el juego", "Could not read the game"));
        gi.data = g_rom_data;
        gi.size = n;
    }
    reload_skin();
    if (!retro_load_game(&gi)) fatal(T("El núcleo no pudo abrir el juego", "The core could not load the game"));
    retro_get_system_av_info(&g_av);
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    load_sram();
    cheats_load();
    if (g_ncheats) cheats_apply();
    vaudio_init(g_av.timing.sample_rate);
    if (!g_skin_on) toast(T("SELECT + START: menú", "SELECT + START: menu"));

    double acc = 0;
    while (!g_quit) {
        SceCtrlData pad;
        uint32_t pressed = read_input(&pad);
        uint32_t nav = ui_nav(pressed);
        switch (g_screen) {
        case SCREEN_MENU: menu_input(nav); break;
        case SCREEN_SAVE: slots_input(nav, true); break;
        case SCREEN_LOAD: slots_input(nav, false); break;
        case SCREEN_CHEATS: cheats_input(nav); break;
        case SCREEN_NONE: {
            game_input(&pad, pressed);
            if (g_screen != SCREEN_NONE) break;
            /* ritmo: tantos fotogramas del núcleo como pida su fps contra los 60 Hz de la pantalla */
            double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 60;
            acc += fps / 59.94;
            int runs = 0;
            while (acc >= 1 && runs < 2) { retro_run(); acc -= 1; runs++; }
            if (g_ff) {
                g_ff_extra = true;
                for (int i = 1; i < g_config.ff_speed; i++) retro_run();
                g_ff_extra = false;
            }
            break;
        }
        }
        ui_begin(g_config.portrait);
        draw_game();
        if (g_skin_on) skin_draw(&g_skin, g_config.opacity * 255 / 100, g_stick_draw_x, g_stick_draw_y);
        if (g_screen == SCREEN_MENU) draw_menu();
        else if (g_screen == SCREEN_SAVE) draw_slots(true);
        else if (g_screen == SCREEN_LOAD) draw_slots(false);
        else if (g_screen == SCREEN_CHEATS) draw_cheats();
        if (g_toast_frames > 0) {
            g_toast_frames--;
            float w = ui_text_w(18, g_toast) + 40;
            ui_round_rect((ui_w() - w) / 2, 24, w, 40, 20, RGBA8(0x20, 0x20, 0x26, 0xE8));
            ui_text_center(ui_w() / 2.0f, 34, COL_TEXT, 18, g_toast);
        }
        ui_end();
    }

    vaudio_quit();
    save_sram();
    save_state("auto"); /* respaldo automático al salir, como Delta */
    retro_unload_game();
    retro_deinit();
    back_to_launcher();
    return 0;
}
