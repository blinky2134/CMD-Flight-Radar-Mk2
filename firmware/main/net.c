#include "net.h"
#include <stdio.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";
static volatile int s_up, s_why;
static char s_ip[16];
static int s_started, s_have_ssid;
static wifi_profiles_t s_profiles;
static int s_profile_idx;
static char s_connected_ssid[33];

/* looking round for networks, for the screen's WiFi page. the driver will not look while it is
 * joining, and with a network set that is not there it joins every two seconds: the joining
 * waits while it looks */
static volatile int s_scan, s_scan_started;       // SCAN_*
static uint32_t     s_scan_asked, s_scan_try;
static ap_t         s_aps[UI_APS];
static int          s_naps;
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

static void apply(const secrets_t *s);

static void scan_collect(int ok)
{
    ap_t got[UI_APS];
    int ng = 0;
    uint16_t n = 0;
    if (ok) esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t r;
    while (n-- && esp_wifi_scan_get_ap_record(&r) == ESP_OK) {   // one at a time: no list of them all in the heap
        const char *name = (const char *)r.ssid;
        if (!name[0]) continue;                                   // a hidden one
        int k = 0;
        while (k < ng && strcmp(got[k].ssid, name)) k++;
        if (k == ng) {
            if (ng < UI_APS) ng++;
            else if (r.rssi <= got[k = ng - 1].rssi) continue;    // full: only a stronger one in place of the weakest
            snprintf(got[k].ssid, sizeof got[k].ssid, "%s", name);
            got[k].rssi = r.rssi;
            got[k].open = r.authmode == WIFI_AUTH_OPEN;
        } else if (r.rssi > got[k].rssi) {
            got[k].rssi = r.rssi;                                 // the same name twice: the nearer one
        }
        while (k > 0 && got[k - 1].rssi < got[k].rssi) {         // strongest first
            ap_t t = got[k]; got[k] = got[k - 1]; got[k - 1] = t;
            k--;
        }
    }
    esp_wifi_clear_ap_list();
    taskENTER_CRITICAL(&s_scan_mux);
    if (ok) {
        memcpy(s_aps, got, sizeof got);
        s_naps = ng;
    }
    s_scan = ok ? SCAN_DONE : SCAN_FAILED;
    s_scan_started = 0;
    taskEXIT_CRITICAL(&s_scan_mux);
    ESP_LOGI(TAG, "looked round: %d networks", ng);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_ssid) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = 0;
        s_ip[0] = 0;
        const wifi_event_sta_disconnected_t *d = data;
        int reason = d ? d->reason : -1;
        // the password is wrong, or there is nobody of that name. the screen says which
        s_why = reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                reason == WIFI_REASON_HANDSHAKE_TIMEOUT ? WHY_PASSWORD : WHY_NOT_FOUND;
        ESP_LOGW(TAG, "WiFi lost (reason %d), trying again", reason);
        vTaskDelay(pdMS_TO_TICKS(2000));              // the event task waits, the router gets a breath
        if (s_scan != SCAN_RUNNING && s_profiles.count) {
            for (int tries = 0; tries < s_profiles.count; tries++) {
                s_profile_idx = (s_profile_idx + 1) % s_profiles.count;
                secrets_t next = { 0 };
                snprintf(next.ssid, sizeof next.ssid, "%s", s_profiles.items[s_profile_idx].ssid);
                snprintf(next.pass, sizeof next.pass, "%s", s_profiles.items[s_profile_idx].pass);
                if (next.ssid[0]) { apply(&next); esp_wifi_connect(); break; }
            }
        } else if (s_have_ssid && s_scan != SCAN_RUNNING) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *e = data;
        scan_collect(e && e->status == 0);
        if (s_have_ssid && !s_up) esp_wifi_connect(); // back to joining
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            snprintf(s_connected_ssid, sizeof s_connected_ssid, "%s", (const char *)ap.ssid);
        s_up = 1;
        s_why = WHY_NONE;
        ESP_LOGI(TAG, "WiFi up, %s", s_ip);
    }
}

void net_scan_request(uint32_t now)
{
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan = SCAN_RUNNING;                            // what it found before stays till the new list
    s_scan_started = 0;
    s_scan_asked = now;
    s_scan_try = now - 1000u;
    taskEXIT_CRITICAL(&s_scan_mux);
}

void net_scan_poll(uint32_t now)
{
    if (s_scan != SCAN_RUNNING) return;
    if (s_scan_started) {
        if (now - s_scan_asked > 20000u) {            // its answer never came
            s_scan = SCAN_FAILED;
            s_scan_started = 0;
            if (s_have_ssid && !s_up) esp_wifi_connect();
        }
        return;
    }
    if (now - s_scan_try < 300u) return;
    s_scan_try = now;
    if (!s_up) esp_wifi_disconnect();                 // a join in the way: stop it, it comes back after
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e == ESP_OK) s_scan_started = 1;
    else if (now - s_scan_asked > 8000u) {
        s_scan = SCAN_FAILED;
        ESP_LOGW(TAG, "could not look round: %s", esp_err_to_name(e));
        if (s_have_ssid && !s_up) esp_wifi_connect();
    }
}

int net_scan_get(ap_t *out, int *n)
{
    taskENTER_CRITICAL(&s_scan_mux);
    memcpy(out, s_aps, sizeof s_aps);
    *n = s_naps;
    int st = s_scan;
    taskEXIT_CRITICAL(&s_scan_mux);
    return st;
}

int net_why(void) { return s_have_ssid && !s_up ? s_why : WHY_NONE; }

static void apply(const secrets_t *s)
{
    s_why = WHY_NONE;                                 // a new network: nothing known about it yet
    wifi_config_t wc = { 0 };
    // the driver's fields need no ending zero when full: a 32 letter name fills all 32
    memcpy(wc.sta.ssid, s->ssid, strnlen(s->ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, s->pass, strnlen(s->pass, sizeof wc.sta.password));
    wc.sta.threshold.authmode = s->pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    s_have_ssid = s->ssid[0] != 0;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void net_start(const secrets_t *s, const wifi_profiles_t *profiles)
{
    if (s_started) return;
    s_started = 1;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    // our NVS keeps the settings, not the driver's
    if (profiles) s_profiles = *profiles;
    if (s_profiles.count > WIFI_PROFILES_MAX) s_profiles.count = WIFI_PROFILES_MAX;
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "OWLOS-Setup");
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (s_profiles.count) {
        s_profile_idx = 0;
        secrets_t first = { 0 };
        snprintf(first.ssid, sizeof first.ssid, "%s", s_profiles.items[0].ssid);
        snprintf(first.pass, sizeof first.pass, "%s", s_profiles.items[0].pass);
        apply(&first);
    } else apply(s);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi started, network \"%s\"", s->ssid);
}

void net_reconnect(const secrets_t *s)
{
    if (!s_started) { net_start(s, NULL); return; }
    if (s->ssid[0]) {
        int idx = -1;
        for (int i = 0; i < s_profiles.count; i++)
            if (!strcmp(s_profiles.items[i].ssid, s->ssid)) { idx = i; break; }
        if (idx < 0 && s_profiles.count < WIFI_PROFILES_MAX) idx = s_profiles.count++;
        if (idx >= 0) {
            snprintf(s_profiles.items[idx].ssid, sizeof s_profiles.items[idx].ssid, "%s", s->ssid);
            snprintf(s_profiles.items[idx].pass, sizeof s_profiles.items[idx].pass, "%s", s->pass);
        }
    }
    s_profile_idx = 0;
    esp_wifi_disconnect();
    apply(s);
    if (s_have_ssid) esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi settings changed, network \"%s\"", s->ssid);
}

void net_set_profiles(const wifi_profiles_t *profiles)
{
    if (!profiles || profiles->count > WIFI_PROFILES_MAX) return;
    s_profiles = *profiles;
    s_profile_idx = 0;
    secrets_t first = { 0 };
    if (s_profiles.count) {
        snprintf(first.ssid, sizeof first.ssid, "%s", s_profiles.items[0].ssid);
        snprintf(first.pass, sizeof first.pass, "%s", s_profiles.items[0].pass);
    }
    net_reconnect(&first);
}

int net_up(void) { return s_up; }

void net_connected_ssid(char *buf, int cap)
{
    snprintf(buf, (size_t)cap, "%s", s_connected_ssid);
}

void net_ip(char *buf, int cap) { snprintf(buf, (size_t)cap, "%s", s_ip); }

int net_get(const char *url, const char *bearer, net_sink_fn sink, void *ctx, int *status)
{
    static char buf[1024];                            // one piece; the fetch task is the only caller
    *status = 0;
    if (!s_up) return -1;
    esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 9000,
        .user_agent = NET_USER_AGENT,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    esp_http_client_set_header(h, "Accept", "application/json");
    if (bearer && bearer[0]) {
        static char auth[640];
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(h, "Authorization", auth);
        esp_http_client_set_header(h, "Accept-Version", "v1");
    }
    int len = -1;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        len = 0;
        while (*status == 200) {
            int r = esp_http_client_read(h, buf, sizeof buf);
            if (r <= 0) break;
            sink(ctx, buf, r);
            len += r;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return len;
}
