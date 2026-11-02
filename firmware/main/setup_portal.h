#pragma once
#include "store.h"

/* Local setup page, served by the board's OWLOS-Setup WiFi access point. */
void setup_portal_start(void);
void setup_get_snapshot(fr_cfg_t *cfg, secrets_t *sec, wifi_profiles_t *profiles);
int setup_save_snapshot(const fr_cfg_t *cfg, const secrets_t *sec, const wifi_profiles_t *profiles);
