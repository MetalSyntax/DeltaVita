#include "common.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/system_param.h>

#define RGBA(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xFF000000u)

/* Mismos colores de acento que la versión Android. */
const SystemInfo SYSTEMS[SYS_COUNT] = {
    [SYS_NES]  = {"nes", "NES", "fceumm", "nes fds", RGBA(0xE5, 0x39, 0x35), false},
    [SYS_SNES] = {"snes", "SNES", "snes9x2005", "sfc smc", RGBA(0x95, 0x75, 0xCD), false},
    [SYS_GBC]  = {"gbc", "GBC", "gambatte", "gb gbc", RGBA(0x8E, 0x24, 0xAA), false},
    [SYS_GBA]  = {"gba", "GBA", "gpsp", "gba", RGBA(0x7C, 0x4D, 0xFF), false},
    [SYS_N64]  = {"n64", "N64", "parallel_n64", "n64 z64 v64", RGBA(0x43, 0xA0, 0x47), true},
    [SYS_DS]   = {"ds", "DS", "desmume2015", "nds", RGBA(0x90, 0xA4, 0xAE), true},
    [SYS_GEN]  = {"genesis", "Genesis", "genesis_plus_gx", "md gen smd bin", RGBA(0x1E, 0x88, 0xE5), false},
};

Config g_config;

int system_for_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return -1;
    char ext[8];
    size_t n = strlen(dot + 1);
    if (n >= sizeof(ext)) return -1;
    for (size_t i = 0; i <= n; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    for (int s = 0; s < SYS_COUNT; s++) {
        const char *p = SYSTEMS[s].exts;
        while (*p) {
            size_t len = strcspn(p, " ");
            if (len == n && strncmp(p, ext, n) == 0) return s;
            p += len;
            while (*p == ' ') p++;
        }
    }
    return -1;
}

static void config_defaults(void) {
    memset(&g_config, 0, sizeof(g_config));
    for (int s = 0; s < SYS_COUNT; s++) g_config.use_skin[s] = 1;
    g_config.opacity = 70;
    g_config.ff_speed = 3;
    g_config.smooth = 1;
    g_config.last_system = SYS_GBA;
}

void config_load(void) {
    config_defaults();
    FILE *f = fopen(DATA_DIR "/config.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        int iv = atoi(v);
        if (!strcmp(line, "portrait")) g_config.portrait = iv;
        else if (!strcmp(line, "opacity")) g_config.opacity = iv < 20 ? 20 : iv > 100 ? 100 : iv;
        else if (!strcmp(line, "ff_speed")) g_config.ff_speed = iv < 2 ? 2 : iv > 8 ? 8 : iv;
        else if (!strcmp(line, "scale_mode")) g_config.scale_mode = iv % 3;
        else if (!strcmp(line, "smooth")) g_config.smooth = iv;
        else if (!strcmp(line, "last_system")) g_config.last_system = iv >= 0 && iv < SYS_COUNT ? iv : 0;
        else for (int s = 0; s < SYS_COUNT; s++) {
            char key[32];
            snprintf(key, sizeof(key), "use_skin_%s", SYSTEMS[s].id);
            if (!strcmp(line, key)) g_config.use_skin[s] = iv;
            snprintf(key, sizeof(key), "skin_%s", SYSTEMS[s].id);
            if (!strcmp(line, key)) snprintf(g_config.skin[s], sizeof(g_config.skin[s]), "%s", v);
        }
    }
    fclose(f);
}

void config_save(void) {
    mkdirs(DATA_DIR);
    FILE *f = fopen(DATA_DIR "/config.txt", "w");
    if (!f) return;
    fprintf(f, "portrait=%d\nopacity=%d\nff_speed=%d\nscale_mode=%d\nsmooth=%d\nlast_system=%d\n",
            g_config.portrait, g_config.opacity, g_config.ff_speed, g_config.scale_mode, g_config.smooth, g_config.last_system);
    for (int s = 0; s < SYS_COUNT; s++)
        fprintf(f, "use_skin_%s=%d\nskin_%s=%s\n", SYSTEMS[s].id, g_config.use_skin[s], SYSTEMS[s].id, g_config.skin[s]);
    fclose(f);
}

bool lang_es(void) {
    static int cached = -1;
    if (cached < 0) {
        SceAppUtilInitParam init = {0};
        SceAppUtilBootParam boot = {0};
        sceAppUtilInit(&init, &boot);
        int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
        sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
        cached = lang == SCE_SYSTEM_PARAM_LANG_SPANISH;
    }
    return cached;
}

void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 5; *p; p++) /* después de "ux0:/" */
        if (*p == '/') { *p = 0; sceIoMkdir(tmp, 0777); *p = '/'; }
    sceIoMkdir(tmp, 0777);
}

bool file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf && size) *size = (size_t)n;
    return buf;
}

bool write_file(const char *path, const void *data, size_t size) {
    /* se escribe a un temporal y se renombra: un corte de batería no deja la partida a medias */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (!ok) { sceIoRemove(tmp); return false; }
    sceIoRemove(path);
    return sceIoRename(tmp, path) >= 0;
}

const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void clean_title(const char *filename, char *out, size_t n) {
    size_t o = 0;
    int depth = 0;
    const char *end = strrchr(filename, '.');
    for (const char *p = filename; *p && p != end && o + 1 < n; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if ((*p == ')' || *p == ']') && depth) depth--;
        else if (!depth) out[o++] = *p;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (!o) snprintf(out, n, "%.*s", end ? (int)(end - filename) : (int)strlen(filename), filename);
}

static FILE *g_log;

void log_init(const char *name) {
    mkdirs(DATA_DIR);
    char path[256];
    snprintf(path, sizeof(path), DATA_DIR "/%s.log", name);
    g_log = fopen(path, "w");
}

void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}
