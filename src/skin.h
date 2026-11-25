/**
 * @file skin.h
 * @brief Skins .deltaskin de Delta (info.json + imágenes) para Vita.
 *
 * Usa la representación "iphone/standard" (667x375 ≈ 16:9, la proporción de la Vita) en horizontal
 * o vertical. Las imágenes deben ser PNG: tools/deltaskin2vita.py convierte las PDF de Delta.
 */
#pragma once
#include <vita2d.h>
#include "common.h"

#define SKIN_MAX_ITEMS 48

typedef enum { ITEM_BUTTONS, ITEM_DPAD, ITEM_STICK, ITEM_TOUCHSCREEN } ItemKind;

typedef struct {
    ItemKind kind;
    char names[4][20];   /**< BUTTONS: inputs ("a", "menu"...); DPAD/STICK: up, down, left, right */
    int count;
    float x, y, w, h;    /**< marco en coordenadas lógicas */
    float hx, hy, hw, hh;/**< zona de toque (con extendedEdges) */
} SkinItem;

typedef struct {
    vita2d_texture *image, *thumb;
    SkinItem items[SKIN_MAX_ITEMS];
    int count;
    float x, y, w, h;      /**< rectángulo de la skin */
    float gx, gy, gw, gh;  /**< donde va la pantalla del juego */
    bool translucent;
    char name[64];
} Skin;

/** Carga la skin para una vista de view_w x view_h. false si no es válida o le faltan PNG. */
bool skin_load(Skin *s, const char *path, int view_w, int view_h);
void skin_free(Skin *s);
/** opacity 0..255 (solo afecta a las translúcidas); stick: desplazamiento -1..1 del stick dibujado. */
void skin_draw(const Skin *s, int opacity, float stick_x, float stick_y);
/** Ítem tocado en (x, y) o -1: primero por su marco, luego por la zona extendida. */
int skin_hit(const Skin *s, float x, float y);
/** Nombre visible guardado en info.json (para listas). */
bool skin_read_name(const char *path, char *out, size_t n);
