/**
 * @file ui.h
 * @brief Dibujo nativo con vita2d: fuente del sistema (PVF), glifos de botones de PlayStation y modo vertical.
 *
 * En vertical todo se dibuja en una textura de 544x960 que se presenta rotada 90°, así que menús,
 * skins y juego usan siempre coordenadas lógicas (ui_w() x ui_h()).
 */
#pragma once
#include <vita2d.h>
#include "common.h"

#ifndef RGBA8 /* vita2d.h ya lo define igual */
#define RGBA8(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#endif
#define COL_BG      RGBA8(0x0E, 0x0E, 0x10, 0xFF)
#define COL_SURFACE RGBA8(0x26, 0x26, 0x2C, 0xFF)
#define COL_TEXT    RGBA8(0xF2, 0xF2, 0xF5, 0xFF)
#define COL_DIM     RGBA8(0x9A, 0x9A, 0xA6, 0xFF)
#define COL_SCRIM   RGBA8(0x00, 0x00, 0x00, 0xA0)

typedef enum { GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_L, GLYPH_R, GLYPH_START, GLYPH_SELECT, GLYPH_DPAD } Glyph;

void ui_init(void);
/** Empieza un fotograma; en vertical dibuja sobre la textura rotada. */
void ui_begin(bool portrait);
void ui_end(void);
int ui_w(void);
int ui_h(void);
bool ui_portrait(void);

/** Cruceta en menús: en vertical gira con la imagen. */
uint32_t ui_nav(uint32_t buttons);
/** Toque de pantalla (coordenadas físicas 960x544) -> coordenadas lógicas. */
void ui_touch_to_logical(float sx, float sy, float *x, float *y);

void ui_rect(float x, float y, float w, float h, uint32_t c);
void ui_round_rect(float x, float y, float w, float h, float r, uint32_t c);
void ui_vgradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
/** Texto con tamaño aproximado en píxeles; y = parte superior. Devuelve el ancho. */
int ui_text(float x, float y, uint32_t c, float px, const char *s);
int ui_text_w(float px, const char *s);
void ui_text_center(float cx, float y, uint32_t c, float px, const char *s);
/** Centrado y recortado con "…" si pasa de max_w. */
void ui_text_center_fit(float cx, float y, uint32_t c, float px, float max_w, const char *s);
/** Texto recortado con "…" para que quepa en max_w. */
void ui_text_fit(float x, float y, uint32_t c, float px, float max_w, const char *s);
/** Botón de PlayStation con su color original (✕ azul, ○ rojo, △ verde, □ rosa). */
void ui_glyph(Glyph g, float cx, float cy, float r);
/** Barra inferior de ayudas: pares (glifo, texto) terminados en -1. */
void ui_hints(uint32_t bg, ...);
/** Textura ajustada a la caja conservando el aspecto. */
void ui_texture_fit(vita2d_texture *t, float x, float y, float w, float h, bool smooth, uint32_t tint);
uint32_t ui_mix(uint32_t a, uint32_t b, float t);
uint32_t ui_alpha(uint32_t c, int a);
