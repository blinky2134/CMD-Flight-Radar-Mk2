#pragma once
#include "ui.h"

/* kept in NVS: what the screen shows (fr_cfg_t), the secrets the site sent, the touch alignment
 * and which screen start-up the board took. the secrets never go to the screen or to the log */
typedef struct {
    char ssid[33];
    char pass[65];
    char key[600];           /* the Flightradar24 API token, they are long */
} secrets_t;

#define WIFI_PROFILES_MAX 5
typedef struct {
    char ssid[33];
    char pass[65];
    char location[81];
    float lat, lon;
    uint8_t resolved;
} wifi_profile_t;
typedef struct {
    uint8_t count;
    wifi_profile_t items[WIFI_PROFILES_MAX];
} wifi_profiles_t;

int  store_load_cfg(fr_cfg_t *c);              // 1 if there was a saved one
void store_save_cfg(const fr_cfg_t *c);
void store_load_secrets(secrets_t *s);         // empty strings where nothing is saved
void store_save_secrets(const secrets_t *s);
int  store_load_profiles(wifi_profiles_t *p);
void store_save_profiles(const wifi_profiles_t *p);
int  store_load_cal(touch_cal_t *cal);
void store_save_cal(const touch_cal_t *cal);
int  store_load_screen(void);                  // -1: never picked
void store_save_screen(int profile);
/* how long this run has lasted, s, saved once a minute: the next start says how long the run
 * before it was, whatever ended it (a reset by the USB line or a power cut too) */
uint32_t store_load_ran(void);
void     store_save_ran(uint32_t s);
