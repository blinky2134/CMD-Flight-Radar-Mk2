#include "setup_portal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "setup";

/* Implemented by main.c so web changes update the live radar and persistent settings. */
extern void setup_get_snapshot(fr_cfg_t *cfg, secrets_t *sec, wifi_profiles_t *profiles);
extern int setup_save_snapshot(const fr_cfg_t *cfg, const secrets_t *sec, const wifi_profiles_t *profiles);
extern void setup_set_screen(int screen);

static const char PAGE[] =
    "<!doctype html><html lang=en><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta name=theme-color content='#07152c'><title>Flight Radar setup</title><style>"
    "*{box-sizing:border-box}body{margin:0 auto;padding:20px;background:#07152c;color:#eaf7ff;font:16px system-ui;max-width:760px}"
    "h1,h2,legend{color:#5ce1ff}p{line-height:1.5;color:#a9c5df}.box,fieldset{padding:16px;border:1px solid #24517c;"
    "border-radius:11px;background:#0b2142;margin:14px 0}label{display:block;margin:12px 0;color:#a9c5df}input,select{display:block;"
    "width:100%;padding:11px;margin-top:5px;border:1px solid #24517c;border-radius:7px;background:#061226;color:white;font-size:16px}"
    "button{padding:12px 16px;background:#5ce1ff;border:0;border-radius:7px;font-weight:bold;font-size:15px;cursor:pointer}"
    "button.remove{background:#351b2c;color:#ff9aaa}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 14px}"
    "small{color:#a9c5df}.rowhead{display:flex;justify-content:space-between;align-items:center;gap:8px}"
    "#status{font-weight:600;color:#7dffa8}#status.bad{color:#ff7888}@media(max-width:560px){.grid{grid-template-columns:1fr}}"
    "</style><h1>Flight Radar setup</h1><p>Join the open WiFi network <b>OWLOS-Setup</b>. Save several WiFi networks with a UK town, address or postcode for each."
    "When the board joins one, it looks up that place with OpenStreetMap and centres the radar there.</p>"
    "<form id=f><section class=box><div class=rowhead><h2>WiFi and home locations</h2><button id=add type=button>Add network</button></div>"
    "<div id=profiles></div><small>Up to five networks. Leave a saved password blank to keep it. Open networks can be marked as open.</small></section>"
    "<section class=box><h2>Aircraft data</h2><label>Source<select id=source><option value=0>Free networks</option>"
    "<option value=1>Flightradar24</option></select></label><label>Flightradar24 API key<input id=key type=password maxlength=599 "
    "placeholder='Leave blank to keep the saved key'></label><small id=keyhint></small></section>"
    "<section class=box><h2>Screen and map</h2><div class=grid>"
    "<label>Map up<select id=up><option value=0>North</option><option value=90>East</option><option value=180>South</option><option value=270>West</option></select></label>"
    "<label>Mounted<select id=flip><option value=0>Right way up</option><option value=1>Upside down</option></select></label>"
    "<label>Colours<select id=theme><option value=0>Arctic</option><option value=1>Phosphor</option><option value=2>Amber</option><option value=3>Crimson</option><option value=4>Synth</option></select></label>"
    "<label>Look<select id=look><option value=0>Radar</option><option value=1>Map with grid</option><option value=2>Classic map</option></select></label>"
    "<label>Map under scope<select id=map><option value=1>On</option><option value=0>Off</option></select></label>"
    "<label>2.8-inch CYD screen<select id=screen><option value=0>V1, micro USB</option><option value=1>V2, USB-C</option><option value=2>V3, ST7789</option></select></label></div></section>"
    "<button type=submit>Save all settings</button></form><p id=status></p><script>"
    "const $=x=>document.getElementById(x), box=$('profiles');let saved=[];"
    "function add(p={}){if(box.children.length>=5)return;const d=document.createElement('fieldset');d.innerHTML=\"<legend>WiFi profile</legend><div class=grid>"
    "<label>Network name<input class=ssid maxlength=32 required></label><label>Password<input class=pass type=password maxlength=64 placeholder='Leave blank to keep saved'></label>"
    "<label>UK town, address or postcode<input class=location maxlength=80 required placeholder='e.g. Edinburgh or EH1 1YZ'></label>"
    "<label><span>Network type</span><select class=open><option value=0>Password protected</option><option value=1>Open network</option></select></label></div>"
    "<button type=button class=remove>Remove this network</button>\";d.querySelector('.ssid').value=p.ssid||'';d.querySelector('.location').value=p.location||'';"
    "d.querySelector('.pass').placeholder=p.hasPass?'Saved; leave blank to keep':'Network password';d.querySelector('.open').value=p.open?'1':'0';"
    "d.querySelector('.remove').onclick=()=>d.remove();box.append(d)}"
    "$('add').onclick=()=>add();$('f').onsubmit=async e=>{e.preventDefault();const status=$('status');status.className='';status.textContent='Saving settings…';"
    "const profiles=[...box.children].map(d=>({ssid:d.querySelector('.ssid').value.trim(),pass:d.querySelector('.pass').value,"
    "location:d.querySelector('.location').value.trim(),open:d.querySelector('.open').value==='1'}));"
    "const payload={profiles,settings:{source:+$('source').value,key:$('key').value,up:+$('up').value,flip:+$('flip').value,"
    "theme:+$('theme').value,look:+$('look').value,map:+$('map').value,screen:+$('screen').value}};try{const r=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});"
    "const j=await r.json();if(!r.ok)throw Error(j.error||'Could not save');status.textContent='Saved. The board will use each place when it joins that WiFi network.';"
    "$('key').value='';for(const d of box.children)d.querySelector('.pass').value='';}catch(x){status.className='bad';status.textContent=x.message}};"
    "fetch('/api/config').then(r=>r.json()).then(c=>{const s=c.settings;$('source').value=s.source;$('up').value=s.up;$('flip').value=s.flip;"
    "$('theme').value=s.theme;$('look').value=s.look;$('map').value=s.map;$('keyhint').textContent=s.keyStored?'A key is already saved. Leave blank to keep it.':'No key saved.';"
    "$('screen').value=s.screen;c.profiles.forEach(p=>add(p));if(!c.profiles.length)add()}).catch(()=>{add();$('status').textContent='Could not read current settings.'});</script></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get(httpd_req_t *req)
{
    fr_cfg_t cfg;
    secrets_t sec;
    wifi_profiles_t profiles;
    setup_get_snapshot(&cfg, &sec, &profiles);
    cJSON *root = cJSON_CreateObject(), *settings = cJSON_CreateObject(), *items = cJSON_CreateArray();
    if (!root || !settings || !items) { cJSON_Delete(root); cJSON_Delete(settings); cJSON_Delete(items); return ESP_FAIL; }
    cJSON_AddItemToObject(root, "settings", settings);
    cJSON_AddItemToObject(root, "profiles", items);
    cJSON_AddNumberToObject(settings, "source", cfg.source);
    cJSON_AddNumberToObject(settings, "up", cfg.up);
    cJSON_AddNumberToObject(settings, "flip", cfg.flip);
    cJSON_AddNumberToObject(settings, "theme", cfg.theme);
    cJSON_AddNumberToObject(settings, "look", cfg.look);
    cJSON_AddNumberToObject(settings, "map", cfg.map);
    cJSON_AddNumberToObject(settings, "screen", store_load_screen() < 0 ? 1 : store_load_screen());
    cJSON_AddBoolToObject(settings, "keyStored", sec.key[0] != 0);
    for (int i = 0; i < profiles.count; i++) {
        cJSON *p = cJSON_CreateObject();
        if (!p) continue;
        cJSON_AddStringToObject(p, "ssid", profiles.items[i].ssid);
        cJSON_AddStringToObject(p, "location", profiles.items[i].location);
        cJSON_AddBoolToObject(p, "hasPass", profiles.items[i].pass[0] != 0);
        cJSON_AddBoolToObject(p, "open", profiles.items[i].pass[0] == 0);
        cJSON_AddItemToArray(items, p);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t api_error(httpd_req_t *req, const char *message, const char *status)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_FAIL;
    cJSON_AddStringToObject(j, "error", message);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return ESP_FAIL;
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static const char *string_field(const cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double number_field(const cJSON *obj, const char *key, double fallback)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static int bool_field(const cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    return cJSON_IsTrue(v);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[3072];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof body)
        return api_error(req, "The settings form is too large.", "413 Payload Too Large");
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return api_error(req, "Could not read the complete form.", "400 Bad Request");
        got += n;
    }
    body[got] = 0;
    cJSON *root = cJSON_Parse(body);
    cJSON *settings = root ? cJSON_GetObjectItemCaseSensitive(root, "settings") : NULL;
    cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "profiles") : NULL;
    if (!cJSON_IsObject(settings) || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > WIFI_PROFILES_MAX) {
        cJSON_Delete(root);
        return api_error(req, "Settings or WiFi profiles are invalid.", "400 Bad Request");
    }

    fr_cfg_t cfg;
    secrets_t sec;
    wifi_profiles_t old_profiles, profiles = { 0 };
    setup_get_snapshot(&cfg, &sec, &old_profiles);
    double source = number_field(settings, "source", cfg.source);
    double up = number_field(settings, "up", cfg.up);
    double flip = number_field(settings, "flip", cfg.flip);
    double theme = number_field(settings, "theme", cfg.theme);
    double look = number_field(settings, "look", cfg.look);
    double map = number_field(settings, "map", cfg.map);
    double screen = number_field(settings, "screen", store_load_screen() < 0 ? 1 : store_load_screen());
    if (source < 0 || source > 1 || up < 0 || up >= 360 || flip < 0 || flip > 1 || theme < 0 || theme >= UI_THEMES ||
        look < 0 || look >= LOOKS || map < 0 || map > 1 || screen < 0 || screen > 2) {
        cJSON_Delete(root);
        return api_error(req, "One of the screen or aircraft settings is invalid.", "400 Bad Request");
    }
    cfg.source = (uint8_t)source;
    cfg.up = (int16_t)up;
    cfg.flip = (uint8_t)flip;
    cfg.theme = (uint8_t)theme;
    cfg.look = (uint8_t)look;
    cfg.map = (uint8_t)map;
    const char *key = string_field(settings, "key");
    if (key && key[0]) {
        if (strlen(key) >= sizeof sec.key) { cJSON_Delete(root); return api_error(req, "The FR24 key is too long.", "400 Bad Request"); }
        snprintf(sec.key, sizeof sec.key, "%s", key);
    }
    if (cfg.source == SRC_FR24 && !sec.key[0]) { cJSON_Delete(root); return api_error(req, "Flightradar24 needs a saved API key.", "400 Bad Request"); }

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        const char *ssid = string_field(item, "ssid"), *pass = string_field(item, "pass"), *location = string_field(item, "location");
        int open = bool_field(item, "open");
        if (!ssid || !ssid[0] || strlen(ssid) > 32 || !location || !location[0] || strlen(location) > 80 ||
            (pass && strlen(pass) > 64) || (pass && pass[0] && !open && (strlen(pass) < 8 || strlen(pass) > 63)) ||
            (!open && (!pass || !pass[0]) && !old_profiles.count)) {
            cJSON_Delete(root);
            return api_error(req, "Each network needs a name and a UK place or postcode.", "400 Bad Request");
        }
        for (int i = 0; i < profiles.count; i++) if (!strcmp(profiles.items[i].ssid, ssid)) {
            cJSON_Delete(root);
            return api_error(req, "Remove duplicate WiFi network names.", "400 Bad Request");
        }
        wifi_profile_t *p = &profiles.items[profiles.count];
        snprintf(p->ssid, sizeof p->ssid, "%s", ssid);
        snprintf(p->location, sizeof p->location, "%s", location);
        int previous = -1;
        for (int i = 0; i < old_profiles.count; i++) if (!strcmp(old_profiles.items[i].ssid, ssid)) { previous = i; break; }
        if (open) p->pass[0] = 0;
        else if (pass && pass[0]) snprintf(p->pass, sizeof p->pass, "%s", pass);
        else if (previous >= 0) snprintf(p->pass, sizeof p->pass, "%s", old_profiles.items[previous].pass);
        else if (!open) { cJSON_Delete(root); return api_error(req, "Enter a password or mark this WiFi as open.", "400 Bad Request"); }
        if (previous >= 0 && !strcmp(old_profiles.items[previous].location, location)) {
            p->lat = old_profiles.items[previous].lat;
            p->lon = old_profiles.items[previous].lon;
            p->resolved = old_profiles.items[previous].resolved;
        }
        profiles.count++;
    }
    cJSON_Delete(root);
    if (!setup_save_snapshot(&cfg, &sec, &profiles)) return api_error(req, "Could not store the settings.", "500 Internal Server Error");
    setup_set_screen((int)screen);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"saved\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Any DNS name requested by a device on the setup AP resolves to the setup page. */
static void dns_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) { close(fd); vTaskDelete(NULL); return; }
    uint8_t packet[512];
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int n = recvfrom(fd, packet, sizeof packet, 0, (struct sockaddr *)&peer, &plen);
        if (n < 17 || (packet[2] & 0x80) || packet[4] != 0 || packet[5] != 1) continue;
        size_t qend = 12;
        while (qend < (size_t)n && packet[qend]) {
            size_t label = packet[qend++];
            if ((label & 0xc0) || qend + label >= (size_t)n) { qend = 0; break; }
            qend += label;
        }
        if (!qend || qend + 5 > (size_t)n || qend + 16 > sizeof packet) continue;
        qend += 5;                                      // root label plus QTYPE and QCLASS
        packet[2] = 0x81; packet[3] = 0x80;             // response, recursion desired/available
        packet[6] = 0; packet[7] = 1;                   // one answer
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        packet[qend++] = 0xc0; packet[qend++] = 0x0c;   // answer name points to the question
        packet[qend++] = 0; packet[qend++] = 1;         // A
        packet[qend++] = 0; packet[qend++] = 1;         // IN
        packet[qend++] = 0; packet[qend++] = 0; packet[qend++] = 0; packet[qend++] = 30;
        packet[qend++] = 0; packet[qend++] = 4;
        packet[qend++] = 192; packet[qend++] = 168; packet[qend++] = 4; packet[qend++] = 1;
        sendto(fd, packet, qend, 0, (struct sockaddr *)&peer, plen);
    }
}

static esp_err_t captive_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

void setup_portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 7;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "could not start setup page");
        return;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t config = { .uri = "/api/config", .method = HTTP_GET, .handler = config_get };
    const httpd_uri_t save = { .uri = "/api/save", .method = HTTP_POST, .handler = save_post };
    const httpd_uri_t probe1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_get };
    const httpd_uri_t probe2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_get };
    const httpd_uri_t probe3 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_get };
    const httpd_uri_t probe4 = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_get };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &config);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &probe1);
    httpd_register_uri_handler(server, &probe2);
    httpd_register_uri_handler(server, &probe3);
    httpd_register_uri_handler(server, &probe4);
    xTaskCreate(dns_task, "setup_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "setup page at http://192.168.4.1/ on WiFi OWLOS-Setup");
}
