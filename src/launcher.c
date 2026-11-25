/**
 * @file launcher.c
 * @brief eboot.bin: biblioteca estilo Delta (una página por consola, carátulas), ajustes y descarga de carátulas.
 *
 * Al elegir un juego escribe ux0:data/DeltaVita/launch.txt y hace LoadExec del núcleo (app0:/<núcleo>_libretro.self).
 */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include "common.h"
#include "skin.h"
#include "ui.h"

#define MAX_GAMES 2000

typedef struct {
    char path[400];
    char title[128];
    int sys;
    vita2d_texture *cover;
    bool cover_tried;
} Game;

static Game *g_games;
static int g_ngames;
static int g_pages[SYS_COUNT], g_npages, g_page;
static int g_sel[SYS_COUNT];
static char g_toast[160];
static int g_toast_frames;
static uint32_t g_prev;

static void toast(const char *m) { snprintf(g_toast, sizeof(g_toast), "%s", m); g_toast_frames = 150; }

/* ---------- títulos propios (renombrar) ---------- */

static void custom_title(const char *name, char *out, size_t n) {
    FILE *f = fopen(DATA_DIR "/titles.txt", "r");
    bool found = false;
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            if (bar && (size_t)(bar - line) == strlen(name) && !strncmp(line, name, bar - line)) {
                snprintf(out, n, "%s", bar + 1);
                found = true;
            }
        }
        fclose(f);
    }
    if (!found) clean_title(name, out, n);
}

static void set_custom_title(const char *name, const char *title) {
    FILE *f = fopen(DATA_DIR "/titles.txt", "a");
    if (!f) return;
    fprintf(f, "%s|%s\n", name, title); /* la última línea gana */
    fclose(f);
}

/* ---------- biblioteca ---------- */

static int cmp_games(const void *a, const void *b) {
    const Game *x = a, *y = b;
    return x->sys != y->sys ? x->sys - y->sys : strcasecmp(x->title, y->title);
}

static void scan_dir(const char *dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0 && g_ngames < MAX_GAMES) {
        if (e.d_name[0] == '.') continue; /* incluye la basura ._ de macOS */
        char path[400];
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) { if (depth < 2) scan_dir(path, depth + 1); continue; }
        int sys = system_for_file(e.d_name);
        if (sys < 0) continue;
        Game *g = &g_games[g_ngames++];
        memset(g, 0, sizeof(*g));
        snprintf(g->path, sizeof(g->path), "%s", path);
        g->sys = sys;
        custom_title(e.d_name, g->title, sizeof(g->title));
    }
    sceIoDclose(d);
}

static void free_covers(void) {
    vita2d_wait_rendering_done();
    for (int i = 0; i < g_ngames; i++) {
        if (g_games[i].cover) vita2d_free_texture(g_games[i].cover);
        g_games[i].cover = NULL;
        g_games[i].cover_tried = false;
    }
}

static void load_library(void) {
    free_covers();
    g_ngames = 0;
    mkdirs(DATA_DIR "/roms");
    scan_dir(DATA_DIR "/roms", 0);
    qsort(g_games, g_ngames, sizeof(Game), cmp_games);
    g_npages = 0;
    for (int s = 0; s < SYS_COUNT; s++)
        for (int i = 0; i < g_ngames; i++) if (g_games[i].sys == s) { g_pages[g_npages++] = s; break; }
    g_page = 0;
    for (int p = 0; p < g_npages; p++) if (g_pages[p] == g_config.last_system) g_page = p;
}

/** Juegos de la página actual: índice del primero y cantidad. */
static int page_games(int *first) {
    if (!g_npages) { *first = 0; return 0; }
    int s = g_pages[g_page], n = 0;
    *first = -1;
    for (int i = 0; i < g_ngames; i++) if (g_games[i].sys == s) { if (*first < 0) *first = i; n++; }
    return n;
}

static void cover_path(const Game *g, char *out, size_t n) { snprintf(out, n, DATA_DIR "/covers/%s.png", base_name(g->path)); }

/* ---------- carátulas (libretro-thumbnails por Wi-Fi) ---------- */

static const char *thumb_repo(const Game *g) {
    switch (g->sys) {
    case SYS_NES: return "Nintendo_-_Nintendo_Entertainment_System";
    case SYS_SNES: return "Nintendo_-_Super_Nintendo_Entertainment_System";
    case SYS_GBC: { const char *dot = strrchr(g->path, '.'); return dot && !strcasecmp(dot, ".gbc") ? "Nintendo_-_Game_Boy_Color" : "Nintendo_-_Game_Boy"; }
    case SYS_GBA: return "Nintendo_-_Game_Boy_Advance";
    case SYS_N64: return "Nintendo_-_Nintendo_64";
    case SYS_DS: return "Nintendo_-_Nintendo_DS";
    default: return "Sega_-_Mega_Drive_-_Genesis";
    }
}

static size_t curl_write(void *p, size_t sz, size_t n, void *f) { return fwrite(p, sz, n, f); }

static bool download_cover(CURL *curl, const Game *g) {
    char stem[256], esc[768], url[1200], tmp[512], dest[512];
    snprintf(stem, sizeof(stem), "%s", base_name(g->path));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    /* libretro-thumbnails cambia &*:/`<>?\|" por _ ; luego se codifica para la URL */
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)stem; *s && o + 4 < sizeof(esc); s++) {
        unsigned char c = strchr("&*:/`<>?\\|\"", *s) ? '_' : *s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~()!'", c)) esc[o++] = c;
        else o += snprintf(esc + o, sizeof(esc) - o, "%%%02X", c);
    }
    esc[o] = 0;
    snprintf(url, sizeof(url), "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s.png", thumb_repo(g), esc);
    cover_path(g, dest, sizeof(dest));
    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    fclose(f);
    if (rc != CURLE_OK || http != 200) { sceIoRemove(tmp); return false; }
    sceIoRemove(dest);
    return sceIoRename(tmp, dest) >= 0;
}

static void download_covers(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static char net_mem[1 * 1024 * 1024];
    SceNetInitParam np = {net_mem, sizeof(net_mem), 0};
    sceNetInit(&np);
    sceNetCtlInit();
    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl = curl_easy_init();
    if (!curl) { toast(T("No se pudo iniciar la red", "Could not start networking")); return; }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    /* ponytail: la Vita no trae un almacén de CA para curl; solo se bajan imágenes públicas */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DeltaVita");
    mkdirs(DATA_DIR "/covers");
    int got = 0, missing = 0, todo = 0;
    for (int i = 0; i < g_ngames; i++) { char p[512]; cover_path(&g_games[i], p, sizeof(p)); if (!file_exists(p)) todo++; }
    int done = 0;
    for (int i = 0; i < g_ngames; i++) {
        char p[512];
        cover_path(&g_games[i], p, sizeof(p));
        if (file_exists(p)) continue;
        ui_begin(g_config.portrait);
        char msg[160];
        snprintf(msg, sizeof(msg), T("Descargando carátulas %d / %d", "Downloading box art %d / %d"), ++done, todo);
        ui_text_center_fit(ui_w() / 2.0f, ui_h() / 2.0f - 30, COL_TEXT, 20, ui_w() - 40, msg);
        ui_text_center_fit(ui_w() / 2.0f, ui_h() / 2.0f + 10, COL_DIM, 16, ui_w() - 40, g_games[i].title);
        ui_round_rect(ui_w() * 0.2f, ui_h() / 2.0f + 50, ui_w() * 0.6f, 8, 4, COL_SURFACE);
        ui_round_rect(ui_w() * 0.2f, ui_h() / 2.0f + 50, ui_w() * 0.6f * done / (todo ? todo : 1), 8, 4, SYSTEMS[g_games[i].sys].color);
        ui_end();
        if (download_cover(curl, &g_games[i])) got++; else missing++;
    }
    curl_easy_cleanup(curl);
    free_covers();
    char msg[160];
    snprintf(msg, sizeof(msg), T("Carátulas: %d nuevas, %d no encontradas", "Box art: %d new, %d not found"), got, missing);
    toast(todo ? msg : T("Carátulas al día", "Box art up to date"));
}

/* ---------- teclado del sistema ---------- */

static bool ime_input(const char *title, const char *initial, char *out, size_t n) {
    static uint16_t t16[64], init16[128], text16[128];
    memset(t16, 0, sizeof(t16)); memset(init16, 0, sizeof(init16)); memset(text16, 0, sizeof(text16));
    for (size_t i = 0; i < 63 && title[i]; i++) t16[i] = (uint8_t)title[i];
    for (size_t i = 0; i < 127 && initial[i]; i++) init16[i] = (uint8_t)initial[i]; /* ponytail: solo ASCII inicial */
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.title = t16;
    p.maxTextLength = 120;
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
    if (!ok) return false;
    size_t o = 0;
    for (int i = 0; text16[i] && o + 4 < n; i++) {
        uint16_t c = text16[i];
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 63); }
        else { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
    }
    out[o] = 0;
    return o > 0;
}

/* ---------- lanzar ---------- */

static void launch(const Game *g) {
    char core[160];
    /* misma disposición que RetroArch: "app0:cores/x.bin" da SCE_APPMGR_ERROR_INVALID_SELF_PATH */
    snprintf(core, sizeof(core), "app0:/%s_libretro.self", SYSTEMS[g->sys].core);
    if (!file_exists(core)) { toast(T("Núcleo no incluido", "Core not included")); return; }
    FILE *f = fopen(DATA_DIR "/launch.txt", "w");
    if (!f) { toast(T("No se pudo escribir en ux0:data", "Could not write to ux0:data")); return; }
    fprintf(f, "%s\n%s\n", g->path, g->title);
    fclose(f);
    g_config.last_system = g->sys;
    config_save();
    /* LoadExec no termina el proceso en el acto: hay que salir aquí, sin volver a dibujar */
    int rc = sceAppMgrLoadExec(core, NULL, NULL);
    logf_("LoadExec %s -> 0x%08X\n", core, rc);
    if (rc < 0) { toast(T("No se pudo abrir el núcleo", "Could not start the core")); return; }
    vita2d_wait_rendering_done();
    sceKernelExitProcess(0);
}

/* ---------- pantallas ---------- */

typedef enum { VIEW_LIBRARY, VIEW_OPTIONS, VIEW_SETTINGS, VIEW_CONFIRM_DELETE } View;
static View g_view;
static int g_opt_sel, g_set_sel;

static int grid_cols(void) { return ui_w() > ui_h() ? 5 : 3; }

static void draw_library(void) {
    int W = ui_w(), H = ui_h();
    uint32_t accent = g_npages ? SYSTEMS[g_pages[g_page]].color : SYSTEMS[SYS_GBA].color;
    ui_vgradient(0, 0, W, H, COL_BG, ui_mix(COL_BG, accent, 0.45f));
    ui_text_center(W / 2.0f, 18, COL_TEXT, 22, g_npages ? SYSTEMS[g_pages[g_page]].label : "DeltaVita");
    if (g_npages > 1) {
        ui_glyph(GLYPH_L, 40, 32, 13);
        ui_glyph(GLYPH_R, W - 40, 32, 13);
        float dx = W / 2.0f - (g_npages - 1) * 9;
        for (int p = 0; p < g_npages; p++) vita2d_draw_fill_circle(dx + p * 18, H - 56, 4, p == g_page ? accent : RGBA8(255, 255, 255, 90));
    }
    int first, n = page_games(&first);
    if (!n) {
        ui_text_center(W / 2.0f, H / 2.0f - 60, COL_TEXT, 22, T("Sin juegos", "No games"));
        ui_text_center_fit(W / 2.0f, H / 2.0f - 20, COL_DIM, 16, W - 40, T("Copia tus ROMs a ux0:data/DeltaVita/roms/", "Copy your ROMs to ux0:data/DeltaVita/roms/"));
        ui_text_center_fit(W / 2.0f, H / 2.0f + 10, COL_DIM, 14, W - 40, ".nes .sfc .gb .gbc .gba .nds .md .bin");
        ui_hints(RGBA8(0, 0, 0, 0x60), GLYPH_START, T("Ajustes", "Settings"), -1);
        return;
    }
    int cols = grid_cols(), s = g_pages[g_page], sel = g_sel[s];
    float pad = 18, cw = (W - 2 * 24 - pad * (cols - 1)) / cols, ch = cw + 44;
    int sel_row = sel / cols, rows_visible = (int)((H - 70 - 70) / (ch + pad));
    if (rows_visible < 1) rows_visible = 1;
    int top_row = sel_row - rows_visible + 1 > 0 ? sel_row - rows_visible + 1 : 0;
    int loads = 0;
    for (int k = 0; k < n; k++) {
        int row = k / cols - top_row;
        if (row < 0 || row >= rows_visible + 1) continue;
        Game *g = &g_games[first + k];
        float x = 24 + (k % cols) * (cw + pad), y = 70 + row * (ch + pad);
        if (y > H - 60) continue;
        if (!g->cover_tried && loads < 2) { /* pocas por fotograma para no trabar la navegación */
            char p[512];
            cover_path(g, p, sizeof(p));
            g->cover = file_exists(p) ? vita2d_load_PNG_file(p) : NULL;
            g->cover_tried = true;
            loads++;
        }
        if (k == sel) ui_round_rect(x - 6, y - 6, cw + 12, cw + 12, 12, RGBA8(255, 255, 255, 230));
        if (g->cover) {
            ui_round_rect(x, y, cw, cw, 8, RGBA8(0, 0, 0, 0x50));
            ui_texture_fit(g->cover, x, y, cw, cw, true, 0xFFFFFFFF);
        } else {
            ui_round_rect(x, y, cw, cw, 8, COL_SURFACE);
            ui_text_center(x + cw / 2, y + cw / 2 - 10, accent, 18, SYSTEMS[g->sys].label);
        }
        ui_text_center_fit(x + cw / 2, y + cw + 8, k == sel ? COL_TEXT : accent, 14, cw, g->title);
    }
    ui_hints(RGBA8(0, 0, 0, 0x60), GLYPH_CROSS, T("Jugar", "Play"), GLYPH_TRIANGLE, T("Opciones", "Options"),
             GLYPH_START, T("Ajustes", "Settings"), -1);
}

static Game *selected_game(void) {
    int first, n = page_games(&first);
    if (!n) return NULL;
    int s = g_pages[g_page];
    if (g_sel[s] >= n) g_sel[s] = n - 1;
    return &g_games[first + g_sel[s]];
}

static void library_input(uint32_t p) {
    int first, n = page_games(&first);
    if (p & SCE_CTRL_START) { g_view = VIEW_SETTINGS; g_set_sel = 0; return; }
    if (!n) return;
    int s = g_pages[g_page], cols = grid_cols();
    if (p & SCE_CTRL_RTRIGGER && g_npages > 1) { g_page = (g_page + 1) % g_npages; }
    if (p & SCE_CTRL_LTRIGGER && g_npages > 1) { g_page = (g_page + g_npages - 1) % g_npages; }
    if (p & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER)) return;
    if (p & SCE_CTRL_RIGHT && g_sel[s] + 1 < n) g_sel[s]++;
    if (p & SCE_CTRL_LEFT && g_sel[s] > 0) g_sel[s]--;
    if (p & SCE_CTRL_DOWN) g_sel[s] = g_sel[s] + cols < n ? g_sel[s] + cols : n - 1;
    if (p & SCE_CTRL_UP && g_sel[s] - cols >= 0) g_sel[s] -= cols;
    if (p & SCE_CTRL_CROSS) launch(selected_game());
    if (p & SCE_CTRL_TRIANGLE) { g_view = VIEW_OPTIONS; g_opt_sel = 0; }
}

/* toque en la cuadrícula: seleccionar y, si ya estaba seleccionado, jugar */
static void library_touch(float x, float y) {
    int first, n = page_games(&first);
    if (!n) return;
    int W = ui_w(), H = ui_h(), cols = grid_cols(), s = g_pages[g_page];
    float pad = 18, cw = (W - 2 * 24 - pad * (cols - 1)) / cols, ch = cw + 44;
    int rows_visible = (int)((H - 70 - 70) / (ch + pad));
    if (rows_visible < 1) rows_visible = 1;
    int top_row = g_sel[s] / cols - rows_visible + 1 > 0 ? g_sel[s] / cols - rows_visible + 1 : 0;
    int col = (int)((x - 24) / (cw + pad)), row = (int)((y - 70) / (ch + pad));
    if (x < 24 || y < 70 || col >= cols) return;
    int k = (row + top_row) * cols + col;
    if (k >= n) return;
    if (k == g_sel[s]) launch(&g_games[first + k]);
    else g_sel[s] = k;
}

static const char *options[] = {"play", "rename", "delete"};

static void draw_options(void) {
    Game *g = selected_game();
    if (!g) { g_view = VIEW_LIBRARY; return; }
    int W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, COL_SCRIM);
    float ph = 230;
    ui_round_rect(0, H - ph, W, ph + 20, 24, RGBA8(0x1C, 0x1C, 0x21, 0xF8));
    ui_text_fit(24, H - ph + 18, COL_TEXT, 20, W - 48, g->title);
    ui_text(24, H - ph + 46, SYSTEMS[g->sys].color, 16, SYSTEMS[g->sys].label);
    const char *labels[] = {T("Jugar", "Play"), T("Renombrar", "Rename"), T("Borrar juego", "Delete game")};
    for (int i = 0; i < 3; i++) {
        float y = H - ph + 76 + i * 36;
        if (i == g_opt_sel) ui_round_rect(16, y - 4, W - 32, 34, 8, ui_mix(COL_SURFACE, SYSTEMS[g->sys].color, 0.45f));
        ui_text(32, y + 4, i == 2 ? RGBA8(0xFF, 0x8A, 0x80, 0xFF) : COL_TEXT, 17, labels[i]);
    }
    ui_hints(RGBA8(0x1C, 0x1C, 0x21, 0xFF), GLYPH_CROSS, T("Elegir", "Select"), GLYPH_CIRCLE, T("Atrás", "Back"), -1);
}

static void options_input(uint32_t p) {
    Game *g = selected_game();
    if (p & SCE_CTRL_DOWN && g_opt_sel < 2) g_opt_sel++;
    if (p & SCE_CTRL_UP && g_opt_sel > 0) g_opt_sel--;
    if (p & SCE_CTRL_CIRCLE) g_view = VIEW_LIBRARY;
    if (!(p & SCE_CTRL_CROSS) || !g) return;
    if (!strcmp(options[g_opt_sel], "play")) launch(g);
    else if (!strcmp(options[g_opt_sel], "rename")) {
        char t[128];
        if (ime_input(T("Nombre del juego", "Game name"), g->title, t, sizeof(t))) {
            set_custom_title(base_name(g->path), t);
            snprintf(g->title, sizeof(g->title), "%s", t);
        }
        g_view = VIEW_LIBRARY;
    } else g_view = VIEW_CONFIRM_DELETE;
}

static void draw_confirm(void) {
    Game *g = selected_game();
    int W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, COL_SCRIM);
    ui_round_rect(W * 0.1f, H / 2.0f - 90, W * 0.8f, 150, 18, RGBA8(0x1C, 0x1C, 0x21, 0xFF));
    char m[200];
    snprintf(m, sizeof(m), T("¿Borrar «%s»?", "Delete \"%s\"?"), g ? g->title : "");
    ui_text_center_fit(W / 2.0f, H / 2.0f - 66, COL_TEXT, 19, W * 0.8f - 32, m);
    ui_text_center_fit(W / 2.0f, H / 2.0f - 30, COL_DIM, 14, W * 0.8f - 32, T("Se borra la ROM; las partidas se conservan.", "The ROM is deleted; saves are kept."));
    ui_hints(RGBA8(0x1C, 0x1C, 0x21, 0xFF), GLYPH_CROSS, T("Borrar", "Delete"), GLYPH_CIRCLE, T("Cancelar", "Cancel"), -1);
}

static void confirm_input(uint32_t p) {
    if (p & SCE_CTRL_CIRCLE) g_view = VIEW_LIBRARY;
    if (p & SCE_CTRL_CROSS) {
        Game *g = selected_game();
        if (g) sceIoRemove(g->path);
        load_library();
        g_view = VIEW_LIBRARY;
        toast(T("Juego borrado", "Game deleted"));
    }
}

/* ---------- ajustes ---------- */

enum { S_ORIENT, S_SKINS_START, S_OPACITY = S_SKINS_START + SYS_COUNT, S_SCALE, S_SMOOTH, S_FF, S_COVERS, S_COUNT };

/** Opciones de skin de un sistema: "" = Standard incluida, nombres de skins/<sistema>/, o "-" = sin skin. */
static int skin_choices(int sys, char names[][96], int max) {
    int n = 0;
    snprintf(names[n++], 96, "-");
    snprintf(names[n++], 96, "%s", "");
    char dir[160];
    snprintf(dir, sizeof(dir), DATA_DIR "/skins/%s", SYSTEMS[sys].id);
    SceUID d = sceIoDopen(dir);
    if (d >= 0) {
        SceIoDirent e;
        while (sceIoDread(d, &e) > 0 && n < max) {
            const char *dot = strrchr(e.d_name, '.');
            if (e.d_name[0] != '.' && dot && !strcasecmp(dot, ".deltaskin")) snprintf(names[n++], 96, "%s", e.d_name);
        }
        sceIoDclose(d);
    }
    return n;
}

static void skin_label(int sys, char *out, size_t n) {
    if (!g_config.use_skin[sys]) { snprintf(out, n, "%s", T("Sin skin", "No skin")); return; }
    if (!g_config.skin[sys][0]) { snprintf(out, n, "Standard (Delta)"); return; }
    char path[256];
    snprintf(path, sizeof(path), DATA_DIR "/skins/%s/%s", SYSTEMS[sys].id, g_config.skin[sys]);
    if (!skin_read_name(path, out, n)) snprintf(out, n, "%s", g_config.skin[sys]);
}

static void setting_change(int row, int dir) {
    if (row == S_ORIENT) g_config.portrait = !g_config.portrait;
    else if (row >= S_SKINS_START && row < S_SKINS_START + SYS_COUNT) {
        int sys = row - S_SKINS_START;
        static char names[24][96];
        int n = skin_choices(sys, names, 24), cur = 0;
        for (int i = 0; i < n; i++)
            if ((i == 0 && !g_config.use_skin[sys]) || (i > 0 && g_config.use_skin[sys] && !strcmp(names[i], g_config.skin[sys]))) cur = i;
        cur = (cur + dir + n) % n;
        g_config.use_skin[sys] = cur != 0;
        snprintf(g_config.skin[sys], sizeof(g_config.skin[sys]), "%s", cur ? names[cur] : "");
    } else if (row == S_OPACITY) g_config.opacity = g_config.opacity + dir * 10 < 20 ? 20 : g_config.opacity + dir * 10 > 100 ? 100 : g_config.opacity + dir * 10;
    else if (row == S_SCALE) g_config.scale_mode = (g_config.scale_mode + dir + 3) % 3;
    else if (row == S_SMOOTH) g_config.smooth = !g_config.smooth;
    else if (row == S_FF) {
        static const int speeds[] = {2, 3, 4, 8};
        int i = 0;
        while (i < 3 && speeds[i] != g_config.ff_speed) i++;
        g_config.ff_speed = speeds[(i + dir + 4) % 4];
    }
    config_save();
}

static void draw_settings(void) {
    int W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, COL_BG);
    ui_text(24, 16, COL_TEXT, 22, T("Ajustes", "Settings"));
    float row_h = 36, y0 = 60;
    int visible = (int)((H - y0 - 50) / row_h);
    int top = g_set_sel - visible + 1 > 0 ? g_set_sel - visible + 1 : 0;
    for (int r = top; r < S_COUNT && r < top + visible; r++) {
        float y = y0 + (r - top) * row_h;
        char label[96], value[128] = "";
        if (r == S_ORIENT) { snprintf(label, sizeof(label), "%s", T("Orientación", "Orientation")); snprintf(value, sizeof(value), "%s", g_config.portrait ? T("Vertical", "Portrait") : T("Horizontal", "Landscape")); }
        else if (r < S_SKINS_START + SYS_COUNT) {
            int sys = r - S_SKINS_START;
            snprintf(label, sizeof(label), "Skin %s%s", SYSTEMS[sys].label, SYSTEMS[sys].experimental ? " (beta)" : "");
            skin_label(sys, value, sizeof(value));
        }
        else if (r == S_OPACITY) { snprintf(label, sizeof(label), "%s", T("Opacidad de la skin", "Skin opacity")); snprintf(value, sizeof(value), "%d %%", g_config.opacity); }
        else if (r == S_SCALE) { snprintf(label, sizeof(label), "%s", T("Escala sin skin", "Scaling without skin")); const char *m[] = {T("Ajustar", "Fit"), T("Entera", "Integer"), T("Estirar", "Stretch")}; snprintf(value, sizeof(value), "%s", m[g_config.scale_mode]); }
        else if (r == S_SMOOTH) { snprintf(label, sizeof(label), "%s", T("Filtro suave", "Smooth filter")); snprintf(value, sizeof(value), "%s", g_config.smooth ? T("Sí", "On") : "No"); }
        else if (r == S_FF) { snprintf(label, sizeof(label), "%s", T("Avance rápido", "Fast forward")); snprintf(value, sizeof(value), "%dx", g_config.ff_speed); }
        else { snprintf(label, sizeof(label), "%s", T("Descargar carátulas", "Download box art")); }
        if (r == g_set_sel) ui_round_rect(12, y - 2, W - 24, row_h - 4, 8, COL_SURFACE);
        /* etiqueta a la izquierda y valor a la derecha, cada uno con su mitad: nunca se pisan */
        float half = (W - 68) / 2.0f;
        ui_text_fit(28, y + 8, COL_TEXT, 16, half, label);
        if (value[0]) {
            float vw = ui_text_w(16, value);
            if (vw > half) vw = half;
            ui_text_fit(W - 40 - vw, y + 8, r == g_set_sel ? SYSTEMS[g_config.last_system].color : COL_DIM, 16, half, value);
        }
    }
    ui_hints(RGBA8(0x1C, 0x1C, 0x21, 0xFF), GLYPH_DPAD, T("Cambiar", "Change"), GLYPH_CROSS, T("Elegir", "Select"),
             GLYPH_CIRCLE, T("Atrás", "Back"), -1);
}

static void settings_input(uint32_t p) {
    if (p & SCE_CTRL_DOWN && g_set_sel + 1 < S_COUNT) g_set_sel++;
    if (p & SCE_CTRL_UP && g_set_sel > 0) g_set_sel--;
    if (p & SCE_CTRL_CIRCLE) { g_view = VIEW_LIBRARY; return; }
    if (p & SCE_CTRL_RIGHT) setting_change(g_set_sel, 1);
    if (p & SCE_CTRL_LEFT) setting_change(g_set_sel, -1);
    if (p & SCE_CTRL_CROSS) {
        if (g_set_sel == S_COVERS) download_covers();
        else setting_change(g_set_sel, 1);
    }
}

int main(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    log_init("launcher");
    config_load();
    ui_init();
    g_games = calloc(MAX_GAMES, sizeof(Game));
    if (!g_games) return 1;
    load_library();
    bool touching = false;
    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = ui_nav(pad.buttons & ~g_prev);
        g_prev = pad.buttons;
        switch (g_view) {
        case VIEW_LIBRARY: library_input(pressed); break;
        case VIEW_OPTIONS: options_input(pressed); break;
        case VIEW_SETTINGS: settings_input(pressed); break;
        case VIEW_CONFIRM_DELETE: confirm_input(pressed); break;
        }
        if (g_view == VIEW_LIBRARY) {
            SceTouchData t;
            sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
            if (t.reportNum && !touching) {
                float x, y;
                ui_touch_to_logical(t.report[0].x / 2.0f, t.report[0].y / 2.0f, &x, &y);
                library_touch(x, y);
            }
            touching = t.reportNum > 0;
        }
        ui_begin(g_config.portrait);
        draw_library();
        if (g_view == VIEW_OPTIONS) draw_options();
        else if (g_view == VIEW_SETTINGS) draw_settings();
        else if (g_view == VIEW_CONFIRM_DELETE) draw_confirm();
        if (g_toast_frames > 0) {
            g_toast_frames--;
            float w = ui_text_w(16, g_toast) + 36;
            if (w > ui_w() - 24) w = ui_w() - 24;
            ui_round_rect((ui_w() - w) / 2, 60, w, 36, 18, RGBA8(0x20, 0x20, 0x26, 0xF0));
            ui_text_center_fit(ui_w() / 2.0f, 70, COL_TEXT, 16, w - 36, g_toast);
        }
        ui_end();
    }
}
