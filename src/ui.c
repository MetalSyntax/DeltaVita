#include "ui.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <psp2/ctrl.h>

static vita2d_pvf *g_font;
static vita2d_texture *g_rt;   /* textura 544x960 para el modo vertical */
static bool g_portrait;
static float g_font_px = 20;   /* alto en px de la fuente a escala 1, medido en ui_init */

void ui_init(void) {
    vita2d_init_advanced(4 * 1024 * 1024); /* vértices de menús con muchos círculos + modo vertical */
    vita2d_set_clear_color(COL_BG);
    vita2d_set_vblank_wait(1);
    g_font = vita2d_load_default_pvf();
    int h = vita2d_pvf_text_height(g_font, 1.0f, "Ag");
    if (h > 0) g_font_px = (float)h;
}

void ui_begin(bool portrait) {
    g_portrait = portrait;
    /* en vertical hay dos escenas por fotograma y el bloque temporal no se vaciaba: se desbordaba.
       ui_end() espera a la GPU, así que aquí ya se puede reutilizar entero */
    vita2d_pool_reset();
    if (portrait && !g_rt)
        g_rt = vita2d_create_empty_texture_rendertarget(SCREEN_H, SCREEN_W, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (portrait && g_rt) {
        vita2d_start_drawing_advanced(g_rt, SCE_GXM_SCENE_FRAGMENT_SET_DEPENDENCY);
        vita2d_clear_screen();
    } else {
        g_portrait = false;
        vita2d_start_drawing();
        vita2d_clear_screen();
    }
}

void ui_end(void) {
    vita2d_end_drawing();
    if (g_portrait) {
        /* arriba de la imagen vertical = borde izquierdo de la pantalla (Vita girada 90° a la derecha) */
        vita2d_start_drawing_advanced(NULL, SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY);
        vita2d_clear_screen();
        vita2d_draw_texture_rotate(g_rt, SCREEN_W / 2.0f, SCREEN_H / 2.0f, (float)(-M_PI / 2));
        vita2d_end_drawing();
    }
    vita2d_wait_rendering_done();
    vita2d_swap_buffers();
}

uint32_t ui_nav(uint32_t b) {
    if (!g_config.portrait) return b;
    uint32_t o = b & ~(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
    if (b & SCE_CTRL_LEFT) o |= SCE_CTRL_UP;
    if (b & SCE_CTRL_UP) o |= SCE_CTRL_RIGHT;
    if (b & SCE_CTRL_RIGHT) o |= SCE_CTRL_DOWN;
    if (b & SCE_CTRL_DOWN) o |= SCE_CTRL_LEFT;
    return o;
}

int ui_w(void) { return g_portrait ? SCREEN_H : SCREEN_W; }
int ui_h(void) { return g_portrait ? SCREEN_W : SCREEN_H; }
bool ui_portrait(void) { return g_portrait; }

void ui_touch_to_logical(float sx, float sy, float *x, float *y) {
    if (g_config.portrait) { *x = SCREEN_H - sy; *y = sx; }
    else { *x = sx; *y = sy; }
}

void ui_rect(float x, float y, float w, float h, uint32_t c) { vita2d_draw_rectangle(x, y, w, h, c); }

void ui_round_rect(float x, float y, float w, float h, float r, uint32_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, c);
    vita2d_draw_rectangle(x, y + r, r, h - 2 * r, c);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, c);
    vita2d_draw_fill_circle(x + r, y + r, r, c);
    vita2d_draw_fill_circle(x + w - r, y + r, r, c);
    vita2d_draw_fill_circle(x + r, y + h - r, r, c);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, c);
}

uint32_t ui_mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int i = 0; i < 32; i += 8) {
        float ca = (float)((a >> i) & 0xFF), cb = (float)((b >> i) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t) << i;
    }
    return out;
}

uint32_t ui_alpha(uint32_t c, int a) { return (c & 0x00FFFFFFu) | ((uint32_t)a << 24); }

void ui_vgradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    const int steps = 48;
    for (int i = 0; i < steps; i++)
        vita2d_draw_rectangle(x, y + h * i / steps, w, h / steps + 1, ui_mix(top, bottom, (float)i / (steps - 1)));
}

int ui_text(float x, float y, uint32_t c, float px, const char *s) {
    float scale = px / g_font_px;
    /* vita2d dibuja desde la línea base */
    vita2d_pvf_draw_text(g_font, (int)x, (int)(y + px * 0.82f), c, scale, s);
    return vita2d_pvf_text_width(g_font, scale, s);
}

int ui_text_w(float px, const char *s) { return vita2d_pvf_text_width(g_font, px / g_font_px, s); }

void ui_text_center(float cx, float y, uint32_t c, float px, const char *s) { ui_text(cx - ui_text_w(px, s) / 2.0f, y, c, px, s); }

void ui_text_center_fit(float cx, float y, uint32_t c, float px, float max_w, const char *s) {
    float w = ui_text_w(px, s);
    if (w > max_w) w = max_w;
    ui_text_fit(cx - w / 2, y, c, px, max_w, s);
}

void ui_text_fit(float x, float y, uint32_t c, float px, float max_w, const char *s) {
    if (ui_text_w(px, s) <= max_w) { ui_text(x, y, c, px, s); return; }
    char buf[256];
    size_t n = strlen(s);
    if (n >= sizeof(buf) - 4) n = sizeof(buf) - 4;
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* no cortar un carácter UTF-8 */
        memcpy(buf, s, n);
        strcpy(buf + n, "…");
        if (ui_text_w(px, buf) <= max_w) break;
    }
    ui_text(x, y, c, px, buf);
}

static void thick_line(float x0, float y0, float x1, float y1, float t, uint32_t c) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    float nx = -dy / len, ny = dx / len;
    for (float o = -t / 2; o <= t / 2; o += 0.5f) vita2d_draw_line(x0 + nx * o, y0 + ny * o, x1 + nx * o, y1 + ny * o, c);
}

/* colores de los símbolos del mando de PlayStation */
#define SONY_CROSS    RGBA8(0x7C, 0xB2, 0xE8, 0xFF)
#define SONY_CIRCLE   RGBA8(0xFF, 0x66, 0x6E, 0xFF)
#define SONY_TRIANGLE RGBA8(0x40, 0xE2, 0xA0, 0xFF)
#define SONY_SQUARE   RGBA8(0xD8, 0x8E, 0xE3, 0xFF)

void ui_glyph(Glyph g, float cx, float cy, float r) {
    const uint32_t face = RGBA8(0x1B, 0x1B, 0x20, 0xFF);
    float t = r * 0.16f, k = r * 0.48f;
    switch (g) {
    case GLYPH_CROSS:
        vita2d_draw_fill_circle(cx, cy, r, face);
        thick_line(cx - k, cy - k, cx + k, cy + k, t, SONY_CROSS);
        thick_line(cx - k, cy + k, cx + k, cy - k, t, SONY_CROSS);
        break;
    case GLYPH_CIRCLE:
        vita2d_draw_fill_circle(cx, cy, r, face);
        vita2d_draw_fill_circle(cx, cy, k + t / 2, SONY_CIRCLE);
        vita2d_draw_fill_circle(cx, cy, k - t / 2, face);
        break;
    case GLYPH_TRIANGLE: {
        vita2d_draw_fill_circle(cx, cy, r, face);
        float top = cy - k, bot = cy + k * 0.7f, half = k * 0.95f;
        thick_line(cx, top, cx - half, bot, t, SONY_TRIANGLE);
        thick_line(cx - half, bot, cx + half, bot, t, SONY_TRIANGLE);
        thick_line(cx + half, bot, cx, top, t, SONY_TRIANGLE);
        break;
    }
    case GLYPH_SQUARE: {
        vita2d_draw_fill_circle(cx, cy, r, face);
        float s = k * 0.85f;
        thick_line(cx - s, cy - s, cx + s, cy - s, t, SONY_SQUARE);
        thick_line(cx + s, cy - s, cx + s, cy + s, t, SONY_SQUARE);
        thick_line(cx + s, cy + s, cx - s, cy + s, t, SONY_SQUARE);
        thick_line(cx - s, cy + s, cx - s, cy - s, t, SONY_SQUARE);
        break;
    }
    case GLYPH_DPAD: {
        float a = r * 0.36f;
        ui_round_rect(cx - a, cy - r, a * 2, r * 2, a * 0.5f, COL_TEXT);
        ui_round_rect(cx - r, cy - a, r * 2, a * 2, a * 0.5f, COL_TEXT);
        break;
    }
    default: { /* L, R, START, SELECT: píldora con texto */
        const char *s = g == GLYPH_L ? "L" : g == GLYPH_R ? "R" : g == GLYPH_START ? "START" : "SELECT";
        float px = g == GLYPH_L || g == GLYPH_R ? r * 1.2f : r * 0.8f;
        float w = ui_text_w(px, s) + r * 1.0f;
        if (w < r * 2) w = r * 2;
        ui_round_rect(cx - w / 2, cy - r * 0.75f, w, r * 1.5f, r * 0.75f, COL_TEXT);
        ui_text_center(cx, cy - px * 0.55f, face, px, s);
    }
    }
}

void ui_hints(uint32_t bg, ...) {
    const float h = 40, y = ui_h() - h;
    ui_rect(0, y, ui_w(), h, bg);
    float x = ui_w() - 16;
    /* se dibujan de derecha a izquierda, como la barra de ayudas del sistema */
    va_list ap;
    struct { int g; const char *s; } items[8];
    int n = 0;
    va_start(ap, bg);
    for (;;) {
        int g = va_arg(ap, int);
        if (g < 0 || n == 8) break;
        items[n].g = g;
        items[n].s = va_arg(ap, const char *);
        n++;
    }
    va_end(ap);
    for (int i = n - 1; i >= 0; i--) {
        float tw = ui_text_w(16, items[i].s);
        x -= tw;
        ui_text(x, y + 12, COL_TEXT, 16, items[i].s);
        float gw = items[i].g >= GLYPH_L && items[i].g <= GLYPH_SELECT ? (items[i].g >= GLYPH_START ? 44 : 22) : 22;
        x -= 8 + gw / 2;
        ui_glyph((Glyph)items[i].g, x, y + h / 2, 10);
        x -= gw / 2 + 16;
    }
}

void ui_texture_fit(vita2d_texture *t, float x, float y, float w, float h, bool smooth, uint32_t tint) {
    if (!t) return;
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    float s = fminf(w / tw, h / th);
    vita2d_texture_set_filters(t, smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT,
                               smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    vita2d_draw_texture_tint_scale(t, x + (w - tw * s) / 2, y + (h - th * s) / 2, s, s, tint);
}
