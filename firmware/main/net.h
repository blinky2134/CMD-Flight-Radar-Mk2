/* net - WiFi as a station and HTTPS GET. the WiFi keeps reconnecting by itself; a request is
 * blocking, one at a time, from the fetch task */
#pragma once
#include "store.h"

// how the board names itself to every server: OSM asks for a name it can tell apart
#define NET_USER_AGENT "OWLOS-Flight-Radar/0.2 (+https://github.com/KirinDenis/OWLOS-FLIGHT-RADAR)"

void net_start(const secrets_t *s, const wifi_profiles_t *profiles);
void net_reconnect(const secrets_t *s);      // new WiFi settings, from the site or the screen
void net_set_profiles(const wifi_profiles_t *profiles);
int  net_up(void);                           // 1 = connected, has an address
void net_connected_ssid(char *buf, int cap);
int  net_why(void);                          // WHY_*: the network set is not up because
void net_ip(char *buf, int cap);
/* the screen's WiFi page: look round for networks. request starts it, poll (each frame) gets
 * it going when the driver lets it, get copies the list and says SCAN_* */
void net_scan_request(uint32_t now);
void net_scan_poll(uint32_t now);
int  net_scan_get(ap_t *out, int *n);
/* GET url, the body goes to sink piece by piece as it arrives (only for status 200).
 * bearer = Authorization token or NULL. returns the bytes read, -1 if no connection or no
 * answer; *status = the HTTP status */
typedef void (*net_sink_fn)(void *ctx, const char *data, int len);
int  net_get(const char *url, const char *bearer, net_sink_fn sink, void *ctx, int *status);
