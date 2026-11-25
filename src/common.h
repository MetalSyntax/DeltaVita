/**
 * @file common.h
 * @brief Tipos y utilidades compartidos por el lanzador (eboot) y los ejecutores de núcleo (app0:/<núcleo>_libretro.self).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DATA_DIR "ux0:data/DeltaVita"
#define SCREEN_W 960
#define SCREEN_H 544

typedef enum { SYS_NES, SYS_SNES, SYS_GBC, SYS_GBA, SYS_N64, SYS_DS, SYS_GEN, SYS_COUNT } SystemId;

typedef struct {
    const char *id;         /**< carpeta y nombre de la skin: "nes", "gba"... */
    const char *label;      /**< nombre visible */
    const char *core;       /**< núcleo libretro = app0:cores/<core>.bin */
    const char *exts;       /**< extensiones separadas por espacios */
    uint32_t color;         /**< acento de la página (RGBA8) */
    bool experimental;      /**< N64/DS: lentos en Vita */
} SystemInfo;

extern const SystemInfo SYSTEMS[SYS_COUNT];

/** Sistema según la extensión del archivo, o -1. */
int system_for_file(const char *name);

/** Ajustes globales (ux0:data/DeltaVita/config.txt). */
typedef struct {
    int portrait;               /**< 1 = Vita en vertical (imagen rotada 90°) */
    int use_skin[SYS_COUNT];    /**< 0 = sin skin: juego a pantalla completa */
    char skin[SYS_COUNT][96];   /**< archivo elegido en skins/<sistema>/ ("" = Standard) */
    int opacity;                /**< 20..100, skins translúcidas en horizontal */
    int ff_speed;               /**< 2..8 */
    int scale_mode;             /**< sin skin: 0 ajustar, 1 entero, 2 estirar */
    int smooth;                 /**< filtro bilineal */
    int last_system;            /**< página abierta en la biblioteca */
} Config;

extern Config g_config;
void config_load(void);
void config_save(void);

/** Idioma de la consola: true = español. */
bool lang_es(void);
#define T(es, en) (lang_es() ? (es) : (en))

/** mkdir -p */
void mkdirs(const char *path);
bool file_exists(const char *path);
/** Lee un archivo entero (malloc). NULL si falla. */
void *read_file(const char *path, size_t *size);
bool write_file(const char *path, const void *data, size_t size);
/** "Zelda (USA) [!].gba" -> "Zelda" */
void clean_title(const char *filename, char *out, size_t n);
/** Nombre de archivo sin carpeta. */
const char *base_name(const char *path);

void log_init(const char *name);
void logf_(const char *fmt, ...);
