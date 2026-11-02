/*
 * OWLOS Flight Radar - the aircraft around home on the ViewOwl screens (core/config.h).
 * core 0: WiFi, the fetch task (asks the server every few seconds, merges the answer into the
 *         sky), the console (settings from the site over the USB line)
 * core 1: the screen task - touch and BOOT in, copy the sky, draw the frame in strips
 * before the site sent any settings it shows the demo sky, so the board is never blank.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "cJSON.h"
#include "console.h"
#include "demo.h"
#include "display.h"
#include "gfx.h"
#include "net.h"
#include "map.h"
#include "sky.h"
#include "store.h"
#include "setup_portal.h"
#include "tiles.h"
#include "touch.h"
#include "ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define FLIGHT_VERSION "0.2"
#define DEMO_BYTES     6144            /* the demo's answer: 12 planes */
#define FREE_EVERY_MS  5000u           /* the free networks allow a request a second */
#define FR24_EVERY_MS  20000u          /* FR24 counts credits: slower */

static const char *TAG = "flight";

static sky_t             s_sky;        // written by the fetch task, under s_lock
static demo_t            s_demo;
static fr_cfg_t          s_cfg;        // the settings in use: the screen changes them, the site too
static secrets_t         s_sec;
static wifi_profiles_t   s_profiles;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_fetch;
static ui_t              s_ui;
static volatile int      s_cfg_new;    // the site sent settings: the screen takes s_cfg
static char              s_server[24];
static int               s_http;
static uint32_t          s_fetched;
static volatile uint32_t s_wifi_try;   // when WiFi settings last came, from the site or the screen
static volatile int      s_demo_on;    // the demo sky is up: no WiFi set, or the one set is not there
static volatile int      s_screen = -1, s_screen_set = -1;   // the 2.8" CYD's screen revision: now, and what the site asks

/* how the last run ended, for a board found standing still: did it start again (and why), or
 * did the picture stop with the board running? the uptime goes to NVS once a minute (store.h):
 * RTC memory lost it when the USB line's reset pulled EN, the site's port may do that. the boot
 * log and GET say it */
static uint32_t           s_ran_s;     // how long the run before this one was, s, to a minute
static uint32_t           s_ran_kept;  // the uptime last saved, s
static const char        *s_reset;     // why it ended

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:               return "other";
    }
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

void setup_get_snapshot(fr_cfg_t *cfg, secrets_t *sec, wifi_profiles_t *profiles)
{
    lock();
    if (cfg) *cfg = s_cfg;
    if (sec) *sec = s_sec;
    if (profiles) *profiles = s_profiles;
    unlock();
}

int setup_save_snapshot(const fr_cfg_t *cfg, const secrets_t *sec, const wifi_profiles_t *profiles)
{
    if (!cfg || !sec || !profiles || profiles->count > WIFI_PROFILES_MAX) return 0;
    fr_cfg_t c = *cfg;
    secrets_t s = *sec;
    wifi_profiles_t p = *profiles;
    if (c.lat < -90 || c.lat > 90 || c.lon < -180 || c.lon > 180) return 0;
    if (p.count) {
        snprintf(s.ssid, sizeof s.ssid, "%s", p.items[0].ssid);
        snprintf(s.pass, sizeof s.pass, "%s", p.items[0].pass);
    } else {
        s.ssid[0] = s.pass[0] = 0;
    }
    lock();
    s_cfg = c;
    s_sec = s;
    s_profiles = p;
    s_cfg_new = 1;
    sky_home(&s_sky, c.lat, c.lon);
    s_sky.n = 0;
    s_sky.status = SKY_CONNECTING;
    s_wifi_try = now_ms();
    unlock();
    store_save_cfg(&c);
    store_save_secrets(&s);
    store_save_profiles(&p);
    net_set_profiles(&p);
    if (s_fetch) xTaskNotifyGive(s_fetch);
    return 1;
}

void setup_set_screen(int screen)
{
    if (screen >= 0 && screen < LCD_PROFILES) s_screen_set = screen;
}

void setup_apply_location(float lat, float lon)
{
    fr_cfg_t c;
    lock();
    c = s_cfg;
    c.lat = lat;
    c.lon = lon;
    c.place[0] = c.country[0] = 0;        // resolve a fresh town and country for the new home
    s_cfg = c;
    s_cfg_new = 1;
    sky_home(&s_sky, lat, lon);
    unlock();
    store_save_cfg(&c);
    if (s_fetch) xTaskNotifyGive(s_fetch);
    ESP_LOGI(TAG, "location saved from setup page: %.5f %.5f", (double)lat, (double)lon);
}

/* --- asking the server --- */

/* the open networks: no key, a request a second at most. latitude, longitude, radius in NM.
 * airplanes.live and adsb.one used to be here: both now answer 403 to anyone not let in */
static const char *const FREE_URL[FREE_SERVERS] = {
    "https://api.adsb.lol/v2/point/%.2f/%.2f/%d",
    "https://opendata.adsb.fi/api/v2/lat/%.2f/lon/%.2f/dist/%d",
};
static const char *const FREE_NAME[FREE_SERVERS] = { "ADSB.LOL", "ADSB.FI" };

/* the server learns where to look, not where you live: home goes out rounded to 0.01 degree,
 * about a kilometre. the scope keeps the exact point, the answer reaches 1.6 times past its rim */
static float vague(float deg) { return roundf(deg * 100.0f) / 100.0f; }

// a bit more than the scope, so planes on their way in show on the rim first. the flat square
// reaches further, into its corners
static float ask_km(const fr_cfg_t *c) { return range_km(c) * (!RADAR_ROUND && c->look != LOOK_RADAR ? 2.4f : 1.6f); }

static void free_url(const fr_cfg_t *c, int server, char *url, int cap)
{
    int nm = (int)ceilf(ask_km(c) / 1.852f);
    if (nm > 250) nm = 250;                           // the most they answer
    snprintf(url, (size_t)cap, FREE_URL[server], (double)vague(c->lat), (double)vague(c->lon), nm);
}

static void fr24_url(const fr_cfg_t *c, char *url, int cap)
{
    float lat = vague(c->lat), lon = vague(c->lon);
    float r = ask_km(c), dlat = r / 110.574f, dlon = r / (111.320f * cosf(lat * 3.14159265f / 180.0f));
    snprintf(url, (size_t)cap, "https://fr24api.flightradar24.com/api/live/flight-positions/full?bounds=%.2f,%.2f,%.2f,%.2f",
             (double)(lat + dlat), (double)(lat - dlat), (double)(lon - dlon), (double)(lon + dlon));
}

static void to_reader(void *ctx, const char *data, int len) { sky_read((sky_reader_t *)ctx, data, len); }

/* --- the ground under the scope: OpenStreetMap's tiles, downloaded once into the flash
 * (tiles.c) and read into a picture of the scope's size (core/map.c) when home or the range
 * changes. the screen task draws it, this one makes it: while it is being made it is not ready,
 * and the scope shows the glass alone for that moment --- */

/* the map's pieces: the IRAM the code leaves free first. it takes 32 bit words only, map.c
 * reads and writes nothing smaller, and TLS and the WiFi cannot use it at all */
static void *iram_first(size_t bytes)
{
#if CONFIG_IDF_TARGET_ESP32
    return heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
#else
    (void)bytes;
    return NULL;                                       // the C3's RAM is one: malloc does it
#endif
}

static map_t      s_map;
static map_plan_t s_plan;
static int        s_map_read;          // tiles that went into s_map
static uint32_t   s_map_try;           // when it last asked for missing ones

static void map_service(const fr_cfg_t *c)
{
    if (!c->map) return;
    const float kpx = ui_ground_kpx(c);
    int w, h, disc;
    ui_ground_size(c, &w, &h, &disc);                       // the look sets its size: the whole screen or the disc
    int same = s_map.ready && s_map.lat == c->lat && s_map.lon == c->lon && s_map.kpx == kpx && s_map.w == w && s_map.h == h;
    if (same && (s_map_read == s_plan.ntiles || now_ms() - s_map_try < 15000u)) return;
    if (!same) map_plan(&s_plan, c->lat, c->lon, kpx, w, h, disc);   // a retry asks for the same tiles again
    s_map_try = now_ms();
    int kept = tiles_fetch(s_plan.tiles, s_plan.ntiles);      // the missing ones, when the WiFi is up
    uint32_t low_fetch = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    if (same && kept <= s_map_read) return;                   // nothing new to show
    if (!map_alloc(&s_map, w, h, disc)) {
        ESP_LOGE(TAG, "map: no memory for %d x %d texels", w, h);
        return;
    }
    int64_t t0 = esp_timer_get_time();
    map_begin(&s_map);
    int read = 0, oom = 0;
    for (int k = 0; k < s_plan.ntiles; k++) {
        tile_src_t src;
        if (!tiles_open(&s_plan.tiles[k], &src)) continue;
        int r = map_tile(&s_map, &s_plan, k, tiles_read, &src);
        read += r == PNG_OK;
        oom |= r == PNG_NO_MEMORY;
    }
    map_end(&s_map, &s_plan);
    if (oom) ESP_LOGW(TAG, "map: no memory to read a tile in, heap %u, largest %u",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_map_read = read;
    ESP_LOGI(TAG, "map: zoom %d, %d of %d tiles, made in %d ms, heap %u, at least %u after the tiles came, %u after reading them",
             s_plan.z, read, s_plan.ntiles, (int)((esp_timer_get_time() - t0) / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)low_fetch, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

/* --- home's town and country, when the site sent none: OpenStreetMap's Nominatim, asked once a
 * home with the same rounded point the plane servers get --- */

typedef struct { char *b; int n, cap; } text_t;

static void to_text(void *ctx, const char *data, int len)
{
    text_t *t = ctx;
    if (len > t->cap - 1 - t->n) len = t->cap - 1 - t->n;
    memcpy(t->b + t->n, data, (size_t)len);
    t->n += len;
    t->b[t->n] = 0;
}

static void geocode(void)
{
    static float asked_lat = 999, asked_lon = 999;
    fr_cfg_t c;
    lock();
    c = s_cfg;
    unlock();
    if ((c.place[0] && c.country[0]) || !net_up() || (c.lat == asked_lat && c.lon == asked_lon)) return;
    asked_lat = c.lat;
    asked_lon = c.lon;
    static char url[160], buf[2048];
    snprintf(url, sizeof url, "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=10&accept-language=en&lat=%.2f&lon=%.2f",
             (double)vague(c.lat), (double)vague(c.lon));
    text_t t = { buf, 0, sizeof buf };
    buf[0] = 0;
    int status = 0;
    net_get(url, NULL, to_text, &t, &status);
    // the most a place can be called, from a city down to a county: the first one there
    static const char *const KIND[] = { "city", "town", "village", "municipality", "hamlet", "county", "state" };
    char place[sizeof c.place] = "", country[sizeof c.country] = "";
    for (int i = 0; i < (int)(sizeof KIND / sizeof KIND[0]) && !place[0]; i++) json_str(buf, KIND[i], place, sizeof place);
    json_str(buf, "country", country, sizeof country);
    ESP_LOGI(TAG, "where home is: HTTP %d, \"%s\", \"%s\"", status, place, country);
    if (status != 200 || (!place[0] && !country[0])) return;
    int same = 0;
    lock();
    if (s_cfg.lat == c.lat && s_cfg.lon == c.lon) {           // still the same home
        if (!s_cfg.place[0]) memcpy(s_cfg.place, place, sizeof place);
        if (!s_cfg.country[0]) memcpy(s_cfg.country, country, sizeof country);
        c = s_cfg;
        s_cfg_new = 1;
        same = 1;
    }
    unlock();
    if (same) store_save_cfg(&c);
}

static void copy_profile_name(char *out, size_t cap, const char *in)
{
    size_t i = 0, n = 0;
    while (in[i] && n + 1 < cap) {
        unsigned char c = (unsigned char)in[i];
        size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 1;
        if (n + len >= cap) break;
        int valid = 1;
        for (size_t k = 1; k < len; k++)
            if (!in[i + k] || ((unsigned char)in[i + k] & 0xc0) != 0x80) valid = 0;
        if (!valid) len = 1;
        memcpy(out + n, in + i, len);
        n += len;
        i += len;
    }
    out[n] = 0;
}

static void apply_profile_home(const wifi_profile_t *p)
{
    fr_cfg_t c;
    char place[40];
    copy_profile_name(place, sizeof place, p->location);
    lock();
    c = s_cfg;
    if (c.lat == p->lat && c.lon == p->lon && !strcmp(c.place, place) && !strcmp(c.country, "UK")) { unlock(); return; }
    c.lat = p->lat;
    c.lon = p->lon;
    snprintf(c.place, sizeof c.place, "%s", place);
    snprintf(c.country, sizeof c.country, "UK");
    s_cfg = c;
    s_cfg_new = 1;
    sky_home(&s_sky, c.lat, c.lon);
    s_sky.n = 0;
    unlock();
    store_save_cfg(&c);
    ESP_LOGI(TAG, "home for this WiFi: %s, %.5f %.5f", p->location, (double)p->lat, (double)p->lon);
}

static void resolve_profile_home(void)
{
    char ssid[33];
    net_connected_ssid(ssid, sizeof ssid);
    if (!ssid[0]) return;
    wifi_profiles_t profiles;
    lock();
    profiles = s_profiles;
    unlock();
    int idx = -1;
    for (int i = 0; i < profiles.count; i++)
        if (!strcmp(profiles.items[i].ssid, ssid)) { idx = i; break; }
    if (idx < 0 || !profiles.items[idx].location[0]) return;
    wifi_profile_t *p = &profiles.items[idx];
    if (p->resolved) { apply_profile_home(p); return; }

    static char tried_ssid[33], tried_location[81];
    static uint32_t tried_at;
    uint32_t now = now_ms();
    if (!strcmp(tried_ssid, ssid) && !strcmp(tried_location, p->location) && now - tried_at < 300000u) return;
    snprintf(tried_ssid, sizeof tried_ssid, "%s", ssid);
    snprintf(tried_location, sizeof tried_location, "%s", p->location);
    tried_at = now;

    static const char hex[] = "0123456789ABCDEF";
    char query[241], url[420];
    size_t n = 0;
    for (const unsigned char *ch = (const unsigned char *)p->location; *ch && n + 4 < sizeof query; ch++) {
        if ((*ch >= 'a' && *ch <= 'z') || (*ch >= 'A' && *ch <= 'Z') || (*ch >= '0' && *ch <= '9') ||
            *ch == '-' || *ch == '_' || *ch == '.' || *ch == '~') query[n++] = (char)*ch;
        else { query[n++] = '%'; query[n++] = hex[*ch >> 4]; query[n++] = hex[*ch & 15]; }
    }
    query[n] = 0;
    snprintf(url, sizeof url, "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&countrycodes=gb&q=%s", query);
    static char response[2048];
    text_t sink = { response, 0, sizeof response };
    response[0] = 0;
    int status = 0;
    net_get(url, NULL, to_text, &sink, &status);
    cJSON *root = cJSON_Parse(response);
    cJSON *result = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    cJSON *lat_json = result ? cJSON_GetObjectItemCaseSensitive(result, "lat") : NULL;
    cJSON *lon_json = result ? cJSON_GetObjectItemCaseSensitive(result, "lon") : NULL;
    char *end_lat = NULL, *end_lon = NULL;
    float lat = lat_json && cJSON_IsString(lat_json) ? strtof(lat_json->valuestring, &end_lat) : NAN;
    float lon = lon_json && cJSON_IsString(lon_json) ? strtof(lon_json->valuestring, &end_lon) : NAN;
    if (!end_lat || *end_lat || !end_lon || *end_lon) lat = lon = NAN;
    int valid = status == 200 && isfinite(lat) && isfinite(lon) && lat >= 49.8f && lat <= 60.9f && lon >= -8.7f && lon <= 1.9f;
    if (valid) {
        wifi_profiles_t saved_profiles;
        wifi_profile_t saved_profile;
        int save = 0;
        lock();
        if (idx < s_profiles.count && !strcmp(s_profiles.items[idx].ssid, ssid) &&
            !strcmp(s_profiles.items[idx].location, tried_location)) {
            s_profiles.items[idx].lat = lat;
            s_profiles.items[idx].lon = lon;
            s_profiles.items[idx].resolved = 1;
            saved_profile = s_profiles.items[idx];
            saved_profiles = s_profiles;
            save = 1;
        }
        unlock();
        if (save) {
            store_save_profiles(&saved_profiles);
            apply_profile_home(&saved_profile);
        }
    } else {
        ESP_LOGW(TAG, "OpenStreetMap could not find UK location '%s' (HTTP %d); will retry later", tried_location, status);
    }
    cJSON_Delete(root);
}

static void fetch_task(void *arg)
{
    (void)arg;
    static sky_reader_t rd;                            // the answer is read into this as it comes
    static char url[200];
    int next = -1;                                     // free server to try, -1 = the one in the settings
    uint32_t down = 0;                                 // since when the WiFi is not there
    for (;;) {
        fr_cfg_t c;
        int configured, fr24;
        static char key[sizeof s_sec.key];
        lock();
        c = s_cfg;
        configured = s_sec.ssid[0] != 0;
        fr24 = c.source == SRC_FR24 && s_sec.key[0];
        memcpy(key, s_sec.key, sizeof key);
        unlock();
        map_service(&c);                              // first: a new look or range shows its map before the planes come
        uint32_t every = FREE_EVERY_MS;
        const int up = net_up();
        if (up) {
            resolve_profile_home();
            lock(); c = s_cfg; unlock();
        }
        int trying = 0;
        if (configured && !up) {
            uint32_t now = now_ms();
            if (!down) down = now;
            trying = now - (s_wifi_try > down ? s_wifi_try : down) < 20000u;   // a router takes a while
        } else {
            down = 0;
        }
        /* no WiFi: none set, or the one set is not there. the demo sky flies then and the screen
         * says so in big letters; the real one comes back with the WiFi. the two never mix */
        const int demo = !configured || (!up && !trying);
        if (demo != s_demo_on) {
            lock();
            s_sky.n = 0;
            s_demo_on = demo;
            unlock();
        }
        if (demo) {
            static uint32_t told;                      // the demo answers like a server: every 5 s
            uint32_t now = now_ms();
            if (!configured || !s_sky.n || now - told >= FREE_EVERY_MS - 50u) {
                char *buf = malloc(DEMO_BYTES);
                if (!buf) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
                told = now;
                int n = demo_json(&s_demo, c.lat, c.lon, range_km(&c), now, buf, DEMO_BYTES);
                lock();
                sky_read_begin(&rd, &s_sky, 0, now);
                sky_read(&rd, buf, n);
                sky_read_end(&rd, &s_sky);
                sky_forget(&s_sky, now);
                snprintf(s_server, sizeof s_server, "DEMO");
                s_fetched = now;
                unlock();
                free(buf);
            }
            if (configured) every = 1000;              // the WiFi set may come back any moment: look each second
        } else if (!up) {
            lock();
            s_sky.status = SKY_CONNECTING;
            snprintf(s_sky.note, sizeof s_sky.note, "CONNECTING");
            unlock();
            every = 1000;
        } else {
            int server = next < 0 ? (c.server < FREE_SERVERS ? c.server : 0) : next;
            if (fr24) fr24_url(&c, url, sizeof url);
            else free_url(&c, server, url, sizeof url);
            static char said[sizeof url];
            if (strcmp(url, said)) {                   // what goes out, once each time it changes (no key in it)
                ESP_LOGI(TAG, "asking %s", url);
                snprintf(said, sizeof said, "%s", url);
            }
            int status = 0;
            int64_t t0 = esp_timer_get_time();
            lock();
            sky_read_begin(&rd, &s_sky, fr24, now_ms());   // only the home is taken from the sky here
            unlock();
            int len = net_get(url, fr24 ? key : NULL, to_reader, &rd, &status);
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            uint32_t now = now_ms();
            int n = -2;
            lock();
            if (len > 0 && status == 200) {
                rd.now = now;                          // heard now; positions keep the time the request went out
                n = sky_read_end(&rd, &s_sky);
                if (n < 0) {
                    s_sky.status = SKY_PARSE;
                    snprintf(s_sky.note, sizeof s_sky.note, "STRANGE ANSWER");
                } else {
                    snprintf(s_server, sizeof s_server, "%s", fr24 ? "FLIGHTRADAR24" : FREE_NAME[server]);
                    s_fetched = now;
                }
            } else {
                s_sky.status = SKY_HTTP;
                if (status) snprintf(s_sky.note, sizeof s_sky.note, "%s HTTP %d", fr24 ? "FR24" : FREE_NAME[server], status);
                else snprintf(s_sky.note, sizeof s_sky.note, "%s NO ANSWER", fr24 ? "FR24" : FREE_NAME[server]);
            }
            s_http = status;
            sky_forget(&s_sky, now);
            int planes = s_sky.n;
            unlock();
            if (!fr24 && n < 0) next = (server + 1) % FREE_SERVERS;   // that one failed: the next one next time
            else if (!fr24) next = server;
            ESP_LOGI(TAG, "%s: HTTP %d, %d bytes in %d ms, %d planes in it, %d kept, %d on the list, heap %u, at least %u since power on",
                     fr24 ? "FR24" : FREE_NAME[server], status, len, ms, rd.total, n, planes,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            if (fr24) every = FR24_EVERY_MS;
            if (status == 429) every *= 3;            // too many requests: slow down
            geocode();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(every)); // a new setting cuts the wait short
    }
}

/* --- settings from the site, over the USB line (console.h) --- */

// a string as JSON wants it: a quote or a backslash in a network name would end it early
static const char *json_esc(const char *s, char *out, int cap)
{
    int n = 0;
    for (; *s && n < cap - 3; s++) {
        if (*s == '"' || *s == '\\') out[n++] = '\\';
        out[n++] = (unsigned char)*s < 32 ? '?' : *s;
    }
    out[n] = 0;
    return out;
}

static void say_cfg(void)
{
    char ssid[70], place[84], country[68];
    lock();
    printf("OWLOS CFG {\"ssid\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"place\":\"%s\",\"country\":\"%s\",\"src\":\"%s\",\"key\":%s,"
           "\"up\":%d,\"flip\":%d,\"theme\":%d,\"look\":%d,\"map\":%d,\"version\":\"%s\","
           "\"uptime\":%u,\"ran\":%u,\"reset\":\"%s\",\"screen\":%d}\n",
           json_esc(s_sec.ssid, ssid, sizeof ssid), (double)s_cfg.lat, (double)s_cfg.lon,
           json_esc(s_cfg.place, place, sizeof place), json_esc(s_cfg.country, country, sizeof country),
           s_cfg.source == SRC_FR24 ? "fr24" : "free", s_sec.key[0] ? "true" : "false", s_cfg.up, s_cfg.flip,
           s_cfg.theme, s_cfg.look, s_cfg.map, FLIGHT_VERSION,
           (unsigned)(now_ms() / 1000u), (unsigned)s_ran_s, s_reset, s_screen);
    unlock();
}

static void on_line(const char *line)
{
    if (!strncmp(line, "OWLOS HELLO", 11)) { printf("OWLOS READY flight-radar %s\n", FLIGHT_VERSION); return; }
    if (!strncmp(line, "OWLOS GET", 9)) { say_cfg(); return; }
    if (strncmp(line, "OWLOS SET ", 10)) return;
    const char *j = line + 10;
    secrets_t sec;
    fr_cfg_t c;
    lock();
    sec = s_sec;
    c = s_cfg;
    unlock();
    double v;
    char src[8];
    int wifi = 0, home = 0;
    if (json_str(j, "ssid", sec.ssid, sizeof sec.ssid)) wifi = 1;
    if (json_str(j, "pass", sec.pass, sizeof sec.pass)) wifi = 1;
    json_str(j, "key", sec.key, sizeof sec.key);
    if (json_num(j, "lat", &v)) { c.lat = (float)v; home = 1; }
    if (json_num(j, "lon", &v)) { c.lon = (float)v; home = 1; }
    int named = json_str(j, "place", c.place, sizeof c.place);
    int land = json_str(j, "country", c.country, sizeof c.country);
    if (home && !named) c.place[0] = 0;                    // a new home without a name: the board asks OSM for it
    if (home && !land) c.country[0] = 0;
    if (json_str(j, "src", src, sizeof src)) c.source = !strcmp(src, "fr24") ? SRC_FR24 : SRC_FREE;
    if (c.source == SRC_FR24 && !sec.key[0]) c.source = SRC_FREE;
    // how the screen shows it: a board without touch has no other way to these
    if (json_num(j, "up", &v)) c.up = (int16_t)((((int)lround(v)) % 360 + 360) % 360);
    if (json_num(j, "flip", &v)) c.flip = v != 0;
    if (json_num(j, "theme", &v) && v >= 0 && v < UI_THEMES) c.theme = (uint8_t)v;
    if (json_num(j, "look", &v) && v >= 0 && v < LOOKS) c.look = (uint8_t)v;
    if (json_num(j, "map", &v)) c.map = v != 0;
#if LCD_PROFILES
    if (json_num(j, "screen", &v) && v >= 0 && v < LCD_PROFILES) s_screen_set = (int)v;   // the 2.8" CYD's revision: 0 v1, 1 v2, 2 v3
#endif
    if (c.lat < -90 || c.lat > 90 || c.lon < -180 || c.lon > 180) { printf("OWLOS ERR home is not on Earth\n"); return; }
    store_save_secrets(&sec);
    store_save_cfg(&c);
    lock();
    s_sec = sec;
    s_cfg = c;
    s_cfg_new = 1;
    if (home) sky_home(&s_sky, c.lat, c.lon);
    if (wifi) { s_sky.n = 0; s_sky.status = SKY_CONNECTING; s_wifi_try = now_ms(); }   // the demo planes go
    unlock();
    if (wifi) net_reconnect(&sec);
    ESP_LOGI(TAG, "settings from the site: network \"%s\", home %.4f %.4f, %s%s", sec.ssid, (double)c.lat, (double)c.lon,
             c.source == SRC_FR24 ? "FR24" : "free networks", sec.key[0] ? ", FR24 key stored" : "");
    printf("OWLOS OK\n");
    xTaskNotifyGive(s_fetch);
}

/* the screen's WiFi page: a network and the password typed for it. they go where the site's go,
 * and the screen's copy of the password is wiped here */
static void wifi_from_screen(void)
{
    secrets_t sec;
    lock();
    sec = s_sec;
    unlock();
    snprintf(sec.ssid, sizeof sec.ssid, "%s", s_ui.wifi_ssid);
    snprintf(sec.pass, sizeof sec.pass, "%s", s_ui.wifi_pass);
    memset(s_ui.wifi_pass, 0, sizeof s_ui.wifi_pass);
    store_save_secrets(&sec);
    lock();
    s_sec = sec;
    s_sky.n = 0;
    s_sky.status = SKY_CONNECTING;
    s_wifi_try = now_ms();
    unlock();
    net_reconnect(&sec);
    memset(sec.pass, 0, sizeof sec.pass);
    ESP_LOGI(TAG, "WiFi from the screen: network \"%s\"", sec.ssid);
    xTaskNotifyGive(s_fetch);
}

/* --- the screen --- */

typedef struct { uint8_t down; int16_t x, y, rx, ry; uint32_t t; } tsample_t;
static tsample_t s_ts[24];
static int       s_nts;
static uint32_t  s_ts_last;
static uint32_t  s_touch_us, s_send_us;   // reading the touch and handing strips over, for the 10 s log

static void touch_sample(int force)
{
    uint32_t now = now_ms();
    if (s_nts >= (int)(sizeof s_ts / sizeof s_ts[0]) || (!force && now - s_ts_last < 15u)) return;
    int rx = 0, ry = 0, x = 0, y = 0;
    uint32_t t0 = now_us();
    int down = tp_read(&rx, &ry);
    s_touch_us += now_us() - t0;
    if (down) touch_apply(&s_ui.cal, rx, ry, &x, &y);
    s_ts[s_nts++] = (tsample_t){ (uint8_t)down, (int16_t)x, (int16_t)y, (int16_t)rx, (int16_t)ry, now };
    s_ts_last = now;
}

/* --- the second core: on the 3.5" in the radar look it draws every other strip while this core
 * draws the one above it, and the two go out in one transfer. its strip, its stack and the DMA
 * buffers' second half are taken only for that look: the square map wants the memory, and there
 * the SPI is what a frame waits for anyway --- */
#if DISP_HELPER
#define HELPER_STACK 7168
static TaskHandle_t      s_helper;
static SemaphoreHandle_t s_help_go, s_help_done;
static canvas_t          s_help_cv;            // the strip it draws now
static volatile int      s_help_stop;
static uint32_t          s_help_wait_us;       // this core waiting for it, for the 10 s log

static void helper_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_help_go, portMAX_DELAY);
        if (s_help_stop) break;
        ui_strip(&s_ui, &s_help_cv);                   // the strips only read what ui_frame set up
        disp_pack_under(&s_help_cv);                   // into the transfer, under this core's strip
        xSemaphoreGive(s_help_done);
    }
    xSemaphoreGive(s_help_done);
    vTaskDelete(NULL);
}

// between frames: on when wanted and the memory is there (with room left for TLS), off otherwise
static void helper_set(int on)
{
    if (on == (s_helper != NULL)) return;
    const size_t px = GFX_W * DISP_STRIP * sizeof(uint32_t), dma = GFX_W * DISP_STRIP * sizeof(uint16_t) * 2;
    if (!on) {
        s_help_stop = 1;
        xSemaphoreGive(s_help_go);
        xSemaphoreTake(s_help_done, portMAX_DELAY);   // out of its loop, it deletes itself
        s_helper = NULL;
        heap_caps_free(s_help_cv.px);
        s_help_cv.px = NULL;
        disp_rows(DISP_STRIP);
        ESP_LOGI(TAG, "the second core stops drawing, heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < px + dma + HELPER_STACK + 48 * 1024) return;
    if (!s_help_go) {
        s_help_go = xSemaphoreCreateBinary();
        s_help_done = xSemaphoreCreateBinary();
    }
    s_help_cv = (canvas_t){ heap_caps_malloc(px, MALLOC_CAP_INTERNAL), 0, DISP_STRIP, 1 };
    if (!s_help_cv.px) return;
    s_help_stop = 0;
    if (!disp_rows(2 * DISP_STRIP) ||
        xTaskCreatePinnedToCore(helper_task, "strips", HELPER_STACK, NULL, 4, &s_helper, 0) != pdPASS) {
        s_helper = NULL;
        disp_rows(DISP_STRIP);
        heap_caps_free(s_help_cv.px);
        s_help_cv.px = NULL;
        return;
    }
    ESP_LOGI(TAG, "the second core draws every other strip, heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
#endif

#if LCD_PROFILES
/* the 2.8" CYD's screen revision (board.h): BOOT in the first seconds, or the site. saved */
static void pick_screen(int screen, uint32_t now, const char *by)
{
    disp_profile(screen);
    store_save_screen(screen);
    s_screen = screen;
    ui_screen_name = disp_profile_name();
    snprintf(s_ui.toast, sizeof s_ui.toast, "SCREEN %s", ui_screen_name);
    s_ui.toast_until = now + 3000u;
    ESP_LOGI(TAG, "%s: the screen is now %s, saved", by, ui_screen_name);
}
#endif

/* the screen turned 180 degrees, from its setup page or from the site. the touch alignment
 * turns with it, so it needs no new crosses */
static void screen_flip(int on)
{
#if DISP_LCD_SPI || DISP_ILI9486
    disp_flip(on);
#endif
#if RADAR_TOUCH_CAL
    touch_cal_t *c = &s_ui.cal;
    c->a = -c->a; c->b = -c->b; c->c = (float)(GFX_W - 1) - c->c;
    c->d = -c->d; c->e = -c->e; c->f = (float)(GFX_H - 1) - c->f;
    store_save_cal(c);
#endif
    (void)on;
}

static void ui_task(void *arg)
{
    (void)arg;
    fr_cfg_t c;
    lock();
    c = s_cfg;
    unlock();
#if LCD_PROFILES
    static const int ORDER[LCD_PROFILES] = LCD_ORDER;  // what BOOT goes through, the first one if none was picked
    int screen = store_load_screen();                  // which CYD revision the screen is
    if (screen < 0 || screen >= LCD_PROFILES) screen = ORDER[0];
    disp_profile(screen);
    s_screen = screen;
#endif
#if DISP_LCD_SPI || DISP_ILI9486
    disp_flip(c.flip);
#endif
    disp_init();
    uint32_t *work = heap_caps_malloc(GFX_W * DISP_STRIP * sizeof(uint32_t), MALLOC_CAP_INTERNAL);
    if (!work || !ui_init(&s_ui, now_ms())) {
        ESP_LOGE(TAG, "not enough memory for the screen, free heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        vTaskDelete(NULL);
    }
    ui_set_cfg(&s_ui, &c);
    s_ui.map = &s_map;                                 // the fetch task makes it, the scope draws it
    ui_clock_us = now_us;                              // the layers get timed, the 10 s log says where it goes
#if LCD_PROFILES
    ui_screen_name = disp_profile_name();              // the screen offers the other ones
#endif
    button_init();
    esp_task_wdt_add(NULL);
    tp_init();
#if RADAR_TOUCH_CAL
    int have = store_load_cal(&s_ui.cal);
    const float want = (float)GFX_W / (float)tp_raw_span();
    float sx = fabsf(s_ui.cal.a) + fabsf(s_ui.cal.b), sy = fabsf(s_ui.cal.d) + fabsf(s_ui.cal.e);
    if (!have || sx < want / 3 || sx > want * 3 || sy < want / 3 || sy > want * 3) ui_start_calibration(&s_ui);
#endif
    ESP_LOGI(TAG, "%s %s, %dx%d, free heap %u", RADAR_NAME, FLIGHT_VERSION, GFX_W, GFX_H,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    uint32_t frames = 0, t_stat = now_ms(), busy_us = 0, dirty = 0;
    int light = 0;
    for (;;) {
        uint32_t now = now_ms();
        ui_cmd_t cmd;
        memset(&cmd, 0, sizeof cmd);

        int ev = button_poll(now);
#if LCD_PROFILES
        /* a short BOOT press starts the screen as the next CYD revision: on the touch alignment
         * page (the first thing after flashing) or in the first seconds after power on, both say
         * so on the screen. held on the alignment page it still skips the alignment */
        if (ev == BTN_SHORT && (s_ui.view == VIEW_CAL || now < UI_SCREEN_PICK_MS)) {
            int at = 0;
            while (at < LCD_PROFILES - 1 && ORDER[at] != screen) at++;
            screen = ORDER[(at + 1) % LCD_PROFILES];
            pick_screen(screen, now, "BOOT");
            ev = 0;
        }
        if (s_screen_set >= 0) {                       // the site says which
            screen = s_screen_set;
            s_screen_set = -1;
            pick_screen(screen, now, "the site");
        }
#endif
        if (ev) ui_button(&s_ui, ev == BTN_LONG ? 2 : 1, &cmd);
        touch_sample(1);
        for (int i = 0; i < s_nts; i++) {
            const tsample_t *s = &s_ts[i];
            ui_touch(&s_ui, s->down, s->x, s->y, s->rx, s->ry, s->t, &cmd);
        }
        s_nts = 0;

        if (s_cfg_new) {                               // from the site: it is the master now
            int was = c.flip;
            lock();
            c = s_cfg;
            s_cfg_new = 0;
            unlock();
            ui_set_cfg(&s_ui, &c);
            if (c.flip != was) screen_flip(c.flip);
        }
        if (cmd.cfg) {
            int flipped = s_ui.cfg.flip != c.flip;
            lock();
            s_ui.cfg.lat = s_cfg.lat;                  // home and its name only come from the site
            s_ui.cfg.lon = s_cfg.lon;
            memcpy(s_ui.cfg.place, s_cfg.place, sizeof s_ui.cfg.place);
            memcpy(s_ui.cfg.country, s_cfg.country, sizeof s_ui.cfg.country);
            s_cfg = s_ui.cfg;
            unlock();
            c = s_ui.cfg;
            dirty = now ? now : 1;
            if (flipped) screen_flip(c.flip);
        }
        if (cmd.refetch) xTaskNotifyGive(s_fetch);
        if (cmd.scan) net_scan_request(now);
        if (cmd.wifi) wifi_from_screen();
        net_scan_poll(now);
        if (dirty && now - dirty > 3000u) {           // holding + is one flash write, not twenty
            store_save_cfg(&s_ui.cfg);
            dirty = 0;
        }
        if (cmd.save_cal) store_save_cal(&s_ui.cal);

        lock();
        memcpy(&s_ui.snap, &s_sky, sizeof s_sky);
        net_t *n = &s_ui.net;
        n->configured = s_sec.ssid[0] != 0;
        n->has_key = s_sec.key[0] != 0;
        n->demo = (uint8_t)s_demo_on;
        n->wifi = (uint8_t)net_up();
        n->why = (uint8_t)net_why();
        snprintf(n->ssid, sizeof n->ssid, "%s", s_sec.ssid);
        snprintf(n->server, sizeof n->server, "%s", s_server);
        n->http = s_http;
        n->fetched = s_fetched;
        unlock();
        net_ip(n->ip, sizeof n->ip);
        int naps = 0;
        n->scan = (uint8_t)net_scan_get(n->aps, &naps);   // what the WiFi page shows
        n->n_aps = (uint8_t)naps;

#if DISP_HELPER
        helper_set(s_ui.view == VIEW_MAIN && s_ui.cfg.look == LOOK_RADAR);
        const int step = s_helper ? 2 * DISP_STRIP : DISP_STRIP;
#else
        const int step = DISP_STRIP;
#endif
        int64_t t0 = esp_timer_get_time();
        ui_frame(&s_ui, now);
        for (int sy0 = 0; sy0 < GFX_H; sy0 += step) {
            canvas_t cv = { work, sy0, sy0 + DISP_STRIP <= GFX_H ? DISP_STRIP : GFX_H - sy0, 0 };
#if DISP_HELPER
            const int y1 = sy0 + DISP_STRIP, two = s_helper && y1 < GFX_H;
            if (two) {                                 // the strip under it, on the other core meanwhile
                s_help_cv.y0 = y1;
                s_help_cv.h = y1 + DISP_STRIP <= GFX_H ? DISP_STRIP : GFX_H - y1;
                xSemaphoreGive(s_help_go);
            }
            ui_strip(&s_ui, &cv);
            if (two) {
                uint32_t tw = now_us();
                xSemaphoreTake(s_help_done, portMAX_DELAY);
                s_help_wait_us += now_us() - tw;
            }
            uint32_t ts = now_us();
            if (two) disp_strip2(&cv, s_help_cv.h);    // both in one transfer
            else disp_strip(&cv);
#else
            ui_strip(&s_ui, &cv);
            uint32_t ts = now_us();
            disp_strip(&cv);                           // packs it to 565, waits for the strip before to be out
#endif
            s_send_us += now_us() - ts;
            touch_sample(0);
        }
        disp_frame_end();
        busy_us += (uint32_t)(esp_timer_get_time() - t0);
        frames++;
        if (light < 100) disp_backlight(light += 10);

        if (now - t_stat >= 10000u) {
            uint32_t f = frames ? frames : 1;
            ESP_LOGI(TAG, "%u fps, %u ms a frame, view %d, %d planes, heap %u", (unsigned)(frames * 1000u / (now - t_stat)),
                     (unsigned)(busy_us / 1000u / f), s_ui.view, s_ui.snap.n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            const uint32_t *pr = s_ui.prof;                // where a frame's time goes, ms: what to make faster
            ESP_LOGI(TAG, "ms a frame: background %u, disc %u, compass %u, planes %u, panel %u, the rest %u, "
                          "sending %u, touch %u",
                     (unsigned)(pr[0] / 1000u / f), (unsigned)(pr[1] / 1000u / f), (unsigned)(pr[2] / 1000u / f),
                     (unsigned)(pr[3] / 1000u / f), (unsigned)(pr[4] / 1000u / f), (unsigned)(pr[5] / 1000u / f),
                     (unsigned)(s_send_us / 1000u / f), (unsigned)(s_touch_us / 1000u / f));
#if DISP_HELPER
            if (s_helper)                              // the layers above are this core's strips only
                ESP_LOGI(TAG, "the second core draws: waited for it %u ms a frame, its stack %u bytes free",
                         (unsigned)(s_help_wait_us / 1000u / f), (unsigned)uxTaskGetStackHighWaterMark(s_helper));
            s_help_wait_us = 0;
#endif
            memset(s_ui.prof, 0, sizeof s_ui.prof);
            if (now / 1000u - s_ran_kept >= 60u) {     // a reset from here on knows how long this run was
                s_ran_kept = now / 1000u;
                store_save_ran(s_ran_kept);
            }
            s_send_us = s_touch_us = 0;
            frames = 0;
            busy_us = 0;
            t_stat = now;
        }
        esp_task_wdt_reset();
        vTaskDelay(1);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_reset = reset_name(esp_reset_reason());
    s_ran_s = store_load_ran();
    store_save_ran(0);
    ESP_LOGI(TAG, "started after: %s; the run before lasted %u s%s", s_reset, (unsigned)s_ran_s,
             s_ran_s ? "" : " (under a minute, or not known)");

    s_lock = xSemaphoreCreateMutex();
    if (!store_load_cfg(&s_cfg)) fr_cfg_defaults(&s_cfg);
    store_load_secrets(&s_sec);
    if (!store_load_profiles(&s_profiles) || !s_profiles.count) {
        if (s_sec.ssid[0]) {
            s_profiles.count = 1;
            snprintf(s_profiles.items[0].ssid, sizeof s_profiles.items[0].ssid, "%s", s_sec.ssid);
            snprintf(s_profiles.items[0].pass, sizeof s_profiles.items[0].pass, "%s", s_sec.pass);
            store_save_profiles(&s_profiles);
        }
    }
    s_demo_on = !s_sec.ssid[0];                         // no WiFi set: the demo from the first frame
    sky_init(&s_sky, s_cfg.lat, s_cfg.lon);
    demo_init(&s_demo, range_km(&s_cfg), esp_random());
    printf("OWLOS READY flight-radar %s\n", FLIGHT_VERSION);
    tiles_init();
    map_mem = iram_first;
    console_start(on_line);
    net_start(&s_sec, &s_profiles);                    // saved networks are tried in turn
    /* the TLS handshake runs on the fetch task, and the map is made there. one core (the C3): it
     * gets the screen's priority, the two take turns a tick each. under the screen it had less
     * than a millisecond a frame, a map took it over ten seconds and IDLE starved into the watchdog */
    xTaskCreatePinnedToCore(fetch_task, "fetch", 12288, NULL, portNUM_PROCESSORS > 1 ? 4 : 5, &s_fetch, 0);
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, portNUM_PROCESSORS > 1 ? 1 : 0);
    setup_portal_start();
}
