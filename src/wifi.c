// The Wi-Fi link, read every second on its own thread (iw takes a few ms): signal, frequency and channel width, link
// rate, network name, the access point (its MAC: on a mesh, which point the Brick is on) and the router.
#include "speed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_mutex *lock;
static WifiInfo cur;

static void gateway(char *out, int n) {   // the default route in /proc/net/route (hex, little-endian)
  FILE *f = fopen("/proc/net/route", "r"); out[0] = 0;
  if (!f) return;
  char line[256], ifc[32]; unsigned dest, gw;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "%31s %x %x", ifc, &dest, &gw) == 3 && dest == 0 && gw) {
      snprintf(out, n, "%u.%u.%u.%u", gw & 255, gw >> 8 & 255, gw >> 16 & 255, gw >> 24);
      break;
    }
  fclose(f);
}

static void read_once(void) {
  WifiInfo w = { 0 };
  FILE *f = popen("/usr/sbin/iw dev wlan0 link 2>/dev/null", "r");
  if (f) {
    char line[200];
    while (fgets(line, sizeof line, f)) {
      char *p;
      if (sscanf(line, "Connected to %19s", w.bssid) == 1) w.connected = 1;
      if ((p = strstr(line, "SSID: "))) { snprintf(w.ssid, sizeof w.ssid, "%s", p + 6); w.ssid[strcspn(w.ssid, "\n")] = 0; }
      if ((p = strstr(line, "freq: "))) w.freq = atoi(p + 6);
      if ((p = strstr(line, "signal: "))) w.dbm = atoi(p + 8);
      if ((p = strstr(line, "tx bitrate: "))) w.rate = atof(p + 12);
    }
    pclose(f);
  }
  if ((f = popen("/usr/sbin/iw dev wlan0 info 2>/dev/null", "r"))) {
    char line[200], *p;
    while (fgets(line, sizeof line, f)) if ((p = strstr(line, "width: "))) w.width = atoi(p + 7);
    pclose(f);
  }
  gateway(w.gateway, sizeof w.gateway);
  SDL_LockMutex(lock);
  w.roams = cur.roams + (w.connected && cur.bssid[0] && strcmp(w.bssid, cur.bssid) != 0);
  if (!w.connected) { w.roams = cur.roams; snprintf(w.bssid, sizeof w.bssid, "%s", cur.bssid); }   // keep the last one
  cur = w;
  SDL_UnlockMutex(lock);
}

static int run(void *ud) {
  (void)ud;
  for (;;) { read_once(); SDL_Delay(1000); }
  return 0;
}

void wifi_start(void) {
  lock = SDL_CreateMutex();
  read_once();
  SDL_Thread *t = SDL_CreateThread(run, "wifi", NULL);
  if (t) SDL_DetachThread(t);
}

void wifi_get(WifiInfo *w) {
  if (!lock) { memset(w, 0, sizeof *w); return; }
  SDL_LockMutex(lock); *w = cur; SDL_UnlockMutex(lock);
}

int wifi_channel(int f) {
  if (f == 2484) return 14;
  if (f >= 2412 && f < 2484) return (f - 2407) / 5;
  if (f >= 5000 && f < 5900) return (f - 5000) / 5;
  if (f >= 5955) return (f - 5950) / 5;
  return 0;
}

const char *wifi_band(int f) { return f >= 5925 ? "6 GHz" : f >= 4900 ? "5 GHz" : f ? "2.4 GHz" : ""; }

void wifi_fake(const WifiInfo *w) {   // --shots: no Wi-Fi in the build container
  if (!lock) lock = SDL_CreateMutex();
  SDL_LockMutex(lock); cur = *w; SDL_UnlockMutex(lock);
}
