#include "skin.h"
#include "ui.h"
#include "third_party/cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

static void *zip_read(zip_t *z, const char *name, size_t *size) {
    zip_stat_t st;
    if (zip_stat(z, name, 0, &st) < 0) return NULL;
    zip_file_t *f = zip_fopen(z, name, 0);
    if (!f) return NULL;
    void *buf = malloc(st.size + 1);
    zip_int64_t n = buf ? zip_fread(f, buf, st.size) : -1;
    zip_fclose(f);
    if (n != (zip_int64_t)st.size) { free(buf); return NULL; }
    ((char *)buf)[st.size] = 0;
    if (size) *size = st.size;
    return buf;
}

static cJSON *read_info(zip_t *z) {
    char *txt = zip_read(z, "info.json", NULL);
    if (!txt) return NULL;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    return j;
}

#define GET(o, k) cJSON_GetObjectItemCaseSensitive((o), (k))
static const char *str(cJSON *o, const char *k) { cJSON *v = GET(o, k); return cJSON_IsString(v) ? v->valuestring : NULL; }

/** PNG de la skin; un asset .pdf se busca como .png del mismo nombre (lo genera deltaskin2vita.py). */
static vita2d_texture *load_image(zip_t *z, const char *name) {
    char png[160];
    snprintf(png, sizeof(png), "%s", name);
    char *dot = strrchr(png, '.');
    if (dot && !strcasecmp(dot, ".pdf")) strcpy(dot, ".png");
    void *buf = zip_read(z, png, NULL);
    if (!buf) { logf_("skin: falta %s\n", png); return NULL; }
    vita2d_texture *t = vita2d_load_PNG_buffer(buf);
    free(buf);
    return t;
}

static float num(cJSON *o, const char *k, float def) {
    cJSON *v = GET(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

bool skin_read_name(const char *path, char *out, size_t n) {
    zip_t *z = zip_open(path, ZIP_RDONLY, NULL);
    if (!z) return false;
    cJSON *info = read_info(z);
    const char *name = info ? str(info, "name") : NULL;
    if (name) snprintf(out, n, "%s", name);
    cJSON_Delete(info);
    zip_close(z);
    return name != NULL;
}

bool skin_load(Skin *s, const char *path, int view_w, int view_h) {
    memset(s, 0, sizeof(*s));
    zip_t *z = zip_open(path, ZIP_RDONLY, NULL);
    if (!z) { logf_("skin: no se abre %s\n", path); return false; }
    cJSON *info = read_info(z);
    bool ok = false;
    if (!info) goto out;
    snprintf(s->name, sizeof(s->name), "%s", str(info, "name") ?: "");

    cJSON *reps = GET(info, "representations");
    cJSON *dev = GET(reps, "iphone");
    if (!dev) dev = GET(reps, "ipad");
    const char *ori = view_w > view_h ? "landscape" : "portrait";
    cJSON *rep = GET(GET(dev, "standard"), ori);
    if (!rep) rep = GET(GET(dev, "edgeToEdge"), ori);
    if (!rep) { logf_("skin: %s sin %s\n", path, ori); goto out; }

    cJSON *ms = GET(rep, "mappingSize");
    float mw = num(ms, "width", 0), mh = num(ms, "height", 0);
    if (mw <= 0 || mh <= 0) goto out;
    float k = fminf(view_w / mw, view_h / mh);
    s->w = mw * k; s->h = mh * k;
    s->x = (view_w - s->w) / 2; s->y = view_h - s->h; /* centrada y pegada abajo, como Delta */
    s->translucent = cJSON_IsTrue(GET(rep, "translucent"));

    cJSON *def_ext = GET(rep, "extendedEdges");
    cJSON *it;
    cJSON_ArrayForEach(it, GET(rep, "items")) {
        if (s->count == SKIN_MAX_ITEMS) break;
        SkinItem *m = &s->items[s->count];
        cJSON *fr = GET(it, "frame");
        m->x = s->x + num(fr, "x", 0) * k; m->y = s->y + num(fr, "y", 0) * k;
        m->w = num(fr, "width", 0) * k; m->h = num(fr, "height", 0) * k;
        cJSON *ext = GET(it, "extendedEdges");
        float el = num(ext, "left", num(def_ext, "left", 0)) * k, et = num(ext, "top", num(def_ext, "top", 0)) * k;
        float er = num(ext, "right", num(def_ext, "right", 0)) * k, eb = num(ext, "bottom", num(def_ext, "bottom", 0)) * k;
        m->hx = m->x - el; m->hy = m->y - et; m->hw = m->w + el + er; m->hh = m->h + et + eb;

        cJSON *in = GET(it, "inputs");
        if (cJSON_IsArray(in)) {
            m->kind = ITEM_BUTTONS;
            cJSON *v;
            cJSON_ArrayForEach(v, in) if (m->count < 4 && cJSON_IsString(v))
                snprintf(m->names[m->count++], sizeof(m->names[0]), "%s", v->valuestring);
        } else if (cJSON_IsObject(in)) {
            static const char *dirs[4] = {"up", "down", "left", "right"};
            const char *x = str(in, "x");
            if (x && !strncmp(x, "touchScreen", 11)) m->kind = ITEM_TOUCHSCREEN;
            else {
                const char *up = str(in, "up");
                m->kind = GET(it, "thumbstick") || (up && !strncmp(up, "analogStick", 11)) ? ITEM_STICK : ITEM_DPAD;
                for (int d = 0; d < 4; d++) {
                    const char *v = str(in, dirs[d]);
                    snprintf(m->names[d], sizeof(m->names[0]), "%s", v ? v : "");
                }
                m->count = 4;
            }
        } else continue;

        cJSON *thumb = GET(it, "thumbstick");
        if (thumb && !s->thumb) {
            const char *tn = str(thumb, "name");
            if (tn) s->thumb = load_image(z, tn);
        }
        s->count++;
    }

    /* pantalla del juego: screens (DS) / gameScreenFrame / espacio libre (o detrás si es translúcida) */
    cJSON *screens = GET(rep, "screens");
    cJSON *gsf = GET(rep, "gameScreenFrame");
    if (cJSON_GetArraySize(screens) > 0) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        cJSON_ArrayForEach(it, screens) {
            cJSON *o = GET(it, "outputFrame");
            float x = num(o, "x", 0), y = num(o, "y", 0);
            x0 = fminf(x0, x); y0 = fminf(y0, y);
            x1 = fmaxf(x1, x + num(o, "width", 0)); y1 = fmaxf(y1, y + num(o, "height", 0));
        }
        s->gx = s->x + x0 * k; s->gy = s->y + y0 * k; s->gw = (x1 - x0) * k; s->gh = (y1 - y0) * k;
    } else if (gsf) {
        s->gx = s->x + num(gsf, "x", 0) * k; s->gy = s->y + num(gsf, "y", 0) * k;
        s->gw = num(gsf, "width", 0) * k; s->gh = num(gsf, "height", 0) * k;
    } else if (view_w < view_h && !s->translucent) {
        s->gx = 0; s->gy = 0; s->gw = view_w; s->gh = s->y;
    } else {
        s->gx = 0; s->gy = 0; s->gw = view_w; s->gh = view_h;
    }

    cJSON *assets = GET(rep, "assets");
    const char *keys[] = {"resizable", "large", "medium", "small"};
    for (int a = 0; a < 4 && !s->image; a++) {
        const char *n = str(assets, keys[a]);
        if (n) s->image = load_image(z, n);
    }
    ok = s->image != NULL;
out:
    cJSON_Delete(info);
    zip_close(z);
    if (!ok) skin_free(s);
    return ok;
}

void skin_free(Skin *s) {
    if (s->image) { vita2d_wait_rendering_done(); vita2d_free_texture(s->image); }
    if (s->thumb) vita2d_free_texture(s->thumb);
    memset(s, 0, sizeof(*s));
}

void skin_draw(const Skin *s, int opacity, float stick_x, float stick_y) {
    if (!s->image) return;
    uint32_t tint = RGBA8(255, 255, 255, s->translucent ? opacity : 255);
    float iw = vita2d_texture_get_width(s->image), ih = vita2d_texture_get_height(s->image);
    vita2d_texture_set_filters(s->image, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_draw_texture_tint_scale(s->image, s->x, s->y, s->w / iw, s->h / ih, tint);
    if (!s->thumb) return;
    for (int i = 0; i < s->count; i++) {
        const SkinItem *m = &s->items[i];
        if (m->kind != ITEM_STICK) continue;
        float tw = vita2d_texture_get_width(s->thumb), th = vita2d_texture_get_height(s->thumb);
        float sc = m->w / tw * 0.95f;
        float cx = m->x + m->w / 2 + stick_x * m->w / 2, cy = m->y + m->h / 2 + stick_y * m->h / 2;
        vita2d_draw_texture_tint_scale(s->thumb, cx - tw * sc / 2, cy - th * sc / 2, sc, sc, tint);
    }
}

static bool in(float x, float y, float rx, float ry, float rw, float rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

int skin_hit(const Skin *s, float x, float y) {
    for (int i = 0; i < s->count; i++)
        if (in(x, y, s->items[i].x, s->items[i].y, s->items[i].w, s->items[i].h)) return i;
    for (int i = 0; i < s->count; i++)
        if (in(x, y, s->items[i].hx, s->items[i].hy, s->items[i].hw, s->items[i].hh)) return i;
    return -1;
}
