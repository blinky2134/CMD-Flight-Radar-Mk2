#include "store.h"
#include <string.h>
#include "nvs.h"

#define NS "flight"        // nvs namespace

static int load_blob(const char *ns, const char *key, void *p, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t got = len;
    esp_err_t e = nvs_get_blob(h, key, p, &got);
    nvs_close(h);
    return e == ESP_OK && got == len;
}

static void save_blob(const char *key, const void *p, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, p, len);
    nvs_commit(h);
    nvs_close(h);
}

static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = 0;
}

/* the settings as version 2 kept them, before the looks, the map and the country. a board
 * that ran it keeps its home, range and colours */
typedef struct {
    uint16_t version;
    uint8_t  source, server, dist_unit, alt_unit, flip, theme, labels;
    int16_t  up;
    float    lat, lon;
    char     place[20];
    float    range;
} cfg_v2_t;

int store_load_cfg(fr_cfg_t *c)
{
    fr_cfg_t t;
    if (load_blob(NS, "cfg", &t, sizeof t) && t.version == FR_CFG_VERSION) {
        *c = t;
        return 1;
    }
    cfg_v2_t o;
    if (!load_blob(NS, "cfg", &o, sizeof o) || o.version != 2) return 0;
    fr_cfg_defaults(c);
    c->source = o.source;
    c->server = o.server;
    c->dist_unit = o.dist_unit;
    c->alt_unit = o.alt_unit;
    c->flip = o.flip;
    c->theme = o.theme;
    c->labels = o.labels;
    c->up = o.up;
    c->lat = o.lat;
    c->lon = o.lon;
    memcpy(c->place, o.place, sizeof o.place);
    c->place[sizeof o.place - 1] = 0;
    c->country[0] = 0;                                   // the board asks OSM for it
    c->range = o.range;
    return 1;
}

void store_save_cfg(const fr_cfg_t *c) { save_blob("cfg", c, sizeof *c); }

void store_load_secrets(secrets_t *s)
{
    memset(s, 0, sizeof *s);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    load_str(h, "ssid", s->ssid, sizeof s->ssid);
    load_str(h, "pass", s->pass, sizeof s->pass);
    load_str(h, "key", s->key, sizeof s->key);
    nvs_close(h);
}

void store_save_secrets(const secrets_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", s->ssid);
    nvs_set_str(h, "pass", s->pass);
    nvs_set_str(h, "key", s->key);
    nvs_commit(h);
    nvs_close(h);
}

int store_load_profiles(wifi_profiles_t *p)
{
    memset(p, 0, sizeof *p);
    if (!load_blob(NS, "wifi_profiles", p, sizeof *p) || p->count > WIFI_PROFILES_MAX) {
        memset(p, 0, sizeof *p);
        return 0;
    }
    for (int i = 0; i < p->count; i++) {
        p->items[i].ssid[sizeof p->items[i].ssid - 1] = 0;
        p->items[i].pass[sizeof p->items[i].pass - 1] = 0;
        p->items[i].location[sizeof p->items[i].location - 1] = 0;
    }
    return 1;
}

void store_save_profiles(const wifi_profiles_t *p)
{
    if (p->count <= WIFI_PROFILES_MAX) save_blob("wifi_profiles", p, sizeof *p);
}

/* a board that ran the WiFi radar or the thermostat already knows its touch: same chip, same
 * panel. so without an alignment of its own it takes theirs */
int store_load_cal(touch_cal_t *cal)
{
    return load_blob(NS, "cal", cal, sizeof *cal) || load_blob("radarscan", "cal", cal, sizeof *cal) ||
           load_blob("thermo", "cal", cal, sizeof *cal);
}

void store_save_cal(const touch_cal_t *cal) { save_blob("cal", cal, sizeof *cal); }

int store_load_screen(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        if (nvs_open("radarscan", NVS_READONLY, &h) != ESP_OK) return -1;   // the radar may know the screen too
    }
    esp_err_t e = nvs_get_u8(h, "screen", &v);
    nvs_close(h);
    return e == ESP_OK ? v : -1;
}

void store_save_screen(int profile)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "screen", (uint8_t)profile);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t store_load_ran(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u32(h, "ran", &v);
    nvs_close(h);
    return v;
}

void store_save_ran(uint32_t s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "ran", s);
    nvs_commit(h);
    nvs_close(h);
}
