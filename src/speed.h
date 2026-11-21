// Speed: how fast the TrimUI Brick's Wi-Fi really is, against the internet (Cloudflare) or the home network
// (speed-server.py on a PC). Drawn in Dot, the shared look of Kino, Musicdisk and Tuner.
#pragma once
#include <SDL2/SDL.h>

#define W 1024
#define H 768
#define ACCENT_SPEED 0xbf7bff   // violet: the app's state dot (testing, walking)

// Buttons: the Brick's face buttons are labelled the Nintendo way round, SDL names them the Xbox way.
enum { BTN_A = SDL_CONTROLLER_BUTTON_B, BTN_B = SDL_CONTROLLER_BUTTON_A, BTN_X = SDL_CONTROLLER_BUTTON_Y,
       BTN_Y = SDL_CONTROLLER_BUTTON_X, BTN_START = SDL_CONTROLLER_BUTTON_START, BTN_SELECT = SDL_CONTROLLER_BUTTON_BACK,
       BTN_MENU = SDL_CONTROLLER_BUTTON_GUIDE, BTN_L1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
       BTN_R1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_UP = SDL_CONTROLLER_BUTTON_DPAD_UP,
       BTN_DOWN = SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
       BTN_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT };

// --- net.c: HTTP/1.1 over TCP, HTTPS with mbedTLS -----------------------------------------------------------------
typedef struct Conn Conn;
int net_init(const char *ca_file);   // the CA list for HTTPS; 0 if it can't be read (HTTPS then fails)
// Opens url (http:// or https://) and sends the request: method, extra header lines ("Name: value\r\n", or NULL),
// a body length (bytes the caller then sends with conn_write; 0 for none). NULL on failure.
Conn *conn_request(const char *method, const char *url, const char *headers, long long body_len);
int conn_write(Conn *c, const void *p, int n);   // < 0 on error
int conn_head(Conn *c);              // reads the status line and headers; returns the status, < 0 on error
int conn_read(Conn *c, void *p, int n);   // body bytes; 0 at the end, < 0 on error
void conn_close(Conn *c);
// GET into a buffer (small answers: JSON); returns the length, < 0 on failure. buf is 0-terminated.
int http_get(const char *url, const char *headers, char *buf, int n);
void conn_abort(Conn *c);            // from another thread: makes its blocked read or write return
int net_rtt(const char *url, double *ms, int n);   // TCP connect round trips to url's server; how many worked
double now_ms(void);

// --- wifi.c: the link, read on its own thread every second --------------------------------------------------------
typedef struct {
  int connected, dbm, freq, width;   // dBm; MHz; channel width MHz
  double rate;                       // link rate, Mbit/s
  char ssid[64], bssid[20];          // access point: its MAC
  char gateway[20];                  // the router (default route)
  int roams;                         // times the access point changed since start
} WifiInfo;
void wifi_start(void);
void wifi_get(WifiInfo *w);
void wifi_fake(const WifiInfo *w);   // --shots
int wifi_channel(int freq);
const char *wifi_band(int freq);     // "2.4 GHz", "5 GHz"

// --- meter.c: the tests, on worker threads -------------------------------------------------------------------------
enum { T_INTERNET, T_SERVER, NTARGETS };
typedef struct {
  int kind, ok;                      // ok: usable (server: set in speed.txt)
  char name[48], detail[96];         // "Internet", "Cloudflare"; "Home server", "192.168.1.20"
  char down[600], up[300], ping[300], headers[400];   // URLs ("" = can't), extra request headers
} Target;
extern Target targets[NTARGETS];
void meter_targets(const char *userdata);   // finds which targets there are (speed.txt)

enum { PH_IDLE, PH_PING, PH_DOWN, PH_UP, PH_DONE, PH_FAIL, PH_WALK };
#define SAMPLES_PER_S 4
#define MAX_SAMPLES 512
typedef struct {
  int phase; char error[96];
  double ping, jitter, router;       // ms; -1 not measured
  double down, up;                   // Mbit/s over the whole phase; -1 not measured
  double low;                        // the slowest second of the download (what makes films stop)
  double live;                       // Mbit/s right now (the last half second)
  float sdown[MAX_SAMPLES], sup[MAX_SAMPLES];   // per 1/SAMPLES_PER_S s
  int ndown, nup;
  double phase_t0, phase_len;        // ms; for the progress of the phase
  // walk: one sample a second, the newest last
  float wmbit[MAX_SAMPLES]; signed char wdbm[MAX_SAMPLES]; unsigned char wroam[MAX_SAMPLES];
  int nwalk;
} Meter;
void meter_start(int target);        // ping, download, upload (where the target has them)
void meter_walk(int target);         // keeps downloading and sampling the Wi-Fi until meter_stop
void meter_stop(void);
void meter_get(Meter *m);            // a copy of the state
void meter_fake(const Meter *m);     // --shots: show this state

// --- history.c ----------------------------------------------------------------------------------------------------
typedef struct {
  long when; int target;
  double down, up, ping, jitter, router, low;
  int dbm, freq; char bssid[20];
} Run;
#define MAX_RUNS 100
void history_open(const char *userdata);
void history_add(const Run *r);
int history_count(void);
const Run *history_get(int i);       // 0 = newest
