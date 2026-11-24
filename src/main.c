// Speed: screens and buttons. Test (ping, download, upload, a graph and what it means for films), Walk (a measurement
// that keeps going while you move around: speed, signal, mesh point changes) and History (earlier runs).
//   speed <userdata> <pak>                     the app
//   speed <userdata> <pak> --shots <dir>       renders every screen with made-up numbers into <dir>/*.bmp
#include "speed.h"
#include "dot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SCR_TEST, SCR_WALK, SCR_HISTORY };
static SDL_Renderer *ren;
static Dot *dot;
static int screen, target, running = 1, hist_sel, hist_top;
static char toast[120]; static Uint32 toast_until;
static Uint32 walk_t0;

static void show_toast(const char *s) { snprintf(toast, sizeof toast, "%s", s); toast_until = SDL_GetTicks() + 2600; }

static void fmt_mbit(double v, char *out, int n) {   // 3 figures: 0.84, 8.40, 84.0, 840
  if (v < 0) snprintf(out, n, "—");
  else if (v < 10) snprintf(out, n, "%.1f", v);
  else snprintf(out, n, "%.0f", v);
}
static void fmt_ap(const char *bssid, char *out, int n) {   // the last three bytes tell mesh points apart
  int l = strlen(bssid);
  snprintf(out, n, l > 8 ? "…%s" : "%s", l > 8 ? bssid + l - 8 : bssid[0] ? bssid : "—");
}

// What the slowest second means for films streamed from home (Kino) (a stream wants about 1.5 x its rate to keep its buffer up)
static const char *verdict(double low, Uint32 *col) {
  if (low >= 12) { *col = DOT_GOOD; return "Smooth for 8 Mbit/s films"; }
  if (low >= 6) { *col = DOT_GOOD; return "Smooth for 4 Mbit/s films"; }
  if (low >= 3) { *col = DOT_WARN; return "Films: use 2 Mbit/s"; }
  *col = DOT_CRITICAL; return "Films will stop to buffer";
}

// --- graph: a dot matrix, one column per sample, lit from the bottom ----------------------------------------------------
// Content 96 to DOT_CONTENT_BOTTOM: the two cards on top (three option rows under a heading), the graph under them.
// The cards share the width (464 each on the Brick, 592 on the Smart Pro); the graph takes what height is left:
// six rows of dots on the Brick, five on the Smart Pro's shorter screen, square-pitched.
#define TOP_H (DOT_PAD + 26 + 20 + 2 * DOT_OPT_PITCH + DOT_OPT_H + DOT_PAD)   // 302
#define GRAPH_Y (DOT_CONTENT_TOP + TOP_H + DOT_GAP)                          // 414
#define CARD_W ((DOT_CONTENT_W - DOT_GAP) / 2)                              // 464 / 592
#define CARD2_X (DOT_MARGIN + CARD_W + DOT_GAP)                             // 520 / 648
#define IN_W (CARD_W - 60)                                                  // inside a card: 404 / 532
#define G_PITCH 18    // across: a column per sample
#define G_HEAD (DOT_PAD + 26 + 20)   // the panel's heading, as dot_panel draws it
static int G_ROWS = 6, G_VPITCH = 22;   // up: the dots of a column
static void graph_fit(void) {
  int room = DOT_CONTENT_BOTTOM - GRAPH_Y - G_HEAD - DOT_PAD;
  if ((6 - 1) * 22 + 10 <= room) G_ROWS = 6, G_VPITCH = 22;
  else G_ROWS = 5, G_VPITCH = 18;
}
static double nice_top(double v) {
  static const double steps[] = { 1, 2, 3, 5, 10, 15, 20, 30, 40, 50, 75, 100, 150, 200, 300, 500, 1000 };
  for (unsigned i = 0; i < sizeof steps / sizeof *steps; i++) if (v <= steps[i]) return steps[i];
  return 2000;
}
static void column(int x, int y, double frac, Uint32 col) {   // y: top of the matrix
  int lit = (int)(frac * G_ROWS + .5);
  if (frac > 0.02 && lit == 0) lit = 1;
  for (int r = 0; r < G_ROWS; r++)
    dot_circle(dot, x, y + (G_ROWS - 1 - r) * G_VPITCH + 5, 5, r < lit ? col : DOT_TRACK);
}

static void graph_panel(const char *label, const char *right, int *x0, int *y0, int *cols) {
  int h = DOT_CONTENT_BOTTOM - GRAPH_Y, gh = (G_ROWS - 1) * G_VPITCH + 10;
  int y = dot_panel(dot, DOT_MARGIN, GRAPH_Y, DOT_CONTENT_W, h, label);
  if (right) dot_text(dot, DOT_PANEL_T, right, DOT_W - DOT_MARGIN - 30, GRAPH_Y + DOT_PAD, DOT_LABEL, DOT_RIGHT, 0);
  *x0 = 75; *y0 = y + (GRAPH_Y + h - DOT_PAD - y - gh) / 2; *cols = (DOT_CONTENT_W - 70) / G_PITCH + 1;
}

// --- test screen --------------------------------------------------------------------------------------------------------
// Three rows; the access point (which mesh point) goes in the heading, the walk's mesh switches in its graph.
static void wifi_card(int walk, const Meter *m) {
  WifiInfo w; wifi_get(&w);
  char s[80], ap[24], head[48];
  fmt_ap(w.bssid, ap, sizeof ap);
  snprintf(head, sizeof head, "%s%s%s", walk ? "Wi-Fi now" : "Wi-Fi", w.connected ? " · " : "", w.connected ? ap : "");
  int P = DOT_OPT_PITCH, y = dot_panel(dot, CARD2_X, DOT_CONTENT_TOP, CARD_W, TOP_H, head);
  int ox = CARD2_X + 30, ow = CARD_W - 60;   // 550, 404 on the Brick
  if (!w.connected) { dot_opt(dot, ox, y, ow, "Signal", "Not connected", 0); return; }
  snprintf(s, sizeof s, "%d dBm", w.dbm); dot_opt(dot, ox, y, ow, "Signal", s, 0);
  // the channel width too when it fits beside the label (it doesn't at 28 px for most): the band matters more
  snprintf(s, sizeof s, "%d · %s · %d MHz", wifi_channel(w.freq), wifi_band(w.freq), w.width);
  if (w.width < 40 || dot_text_w(dot, DOT_VALUE, s) + dot_text_w(dot, DOT_UI, "Channel") + 24 > ow)
    snprintf(s, sizeof s, "%d · %s", wifi_channel(w.freq), wifi_band(w.freq));
  dot_opt(dot, ox, y + P, ow, "Channel", s, 0);
  if (walk) { snprintf(s, sizeof s, "%d", w.roams); dot_opt(dot, ox, y + 2 * P, ow, "Mesh switches", s, 0); return; }
  if (m->router >= 0) snprintf(s, sizeof s, "%.0f ms", m->router); else snprintf(s, sizeof s, "%.0f Mbit/s", w.rate);
  dot_opt(dot, ox, y + 2 * P, ow, m->router >= 0 ? "Router ping" : "Link rate", s, 0);
}

static void big_number(const char *label, int adot, const char *num, const char *unit) {
  dot_card(dot, DOT_MARGIN, DOT_CONTENT_TOP, CARD_W, TOP_H);
  int x = 70;
  if (adot) { dot_adot(dot, 70, DOT_CONTENT_TOP + DOT_PAD + 6, adot == 1 ? dot_accent(dot) : (Uint32)adot); x = 96; }
  dot_text(dot, DOT_PANEL_T, label, x, DOT_CONTENT_TOP + DOT_PAD, DOT_LABEL, DOT_LEFT, 0);   // as the Wi-Fi card's heading
  int w = dot_text(dot, DOT_DISPLAY_XL, num, 64, 156, DOT_TEXT, DOT_LEFT, 0);
  if (unit) dot_text(dot, DOT_TITLE, unit, 64 + w + 14, 206, DOT_LABEL, DOT_LEFT, 0);
}

static void draw_test(void) {
  Meter m; meter_get(&m);
  const Target *t = &targets[target];
  char hdr[160]; snprintf(hdr, sizeof hdr, "%s · %s", t->name, t->detail);
  dot_header(dot, "SPEED", hdr);
  char num[16], s[120];
  int busy = m.phase == PH_PING || m.phase == PH_DOWN || m.phase == PH_UP;
  if (m.phase == PH_IDLE) {
    big_number("READY", 0, "—", "Mbit/s");
    dot_text(dot, DOT_BODY_S, t->ok ? "Press A to measure" : t->detail, 70, 300, t->ok ? DOT_LABEL : DOT_WARN, DOT_LEFT, IN_W);
  } else if (m.phase == PH_FAIL) {
    big_number("FAILED", DOT_CRITICAL, "—", "Mbit/s");
    dot_text(dot, DOT_BODY_S, m.error, 70, 300, DOT_CRITICAL, DOT_LEFT, IN_W);
  } else {
    const char *lab = m.phase == PH_PING ? "PING" : m.phase == PH_DOWN ? "DOWNLOAD" : m.phase == PH_UP ? "UPLOAD" : "DOWNLOAD";
    if (m.phase == PH_PING) big_number(lab, 1, m.ping >= 0 ? (snprintf(num, sizeof num, "%.0f", m.ping), num) : "…", "ms");
    else { fmt_mbit(busy ? m.live : m.down, num, sizeof num); big_number(lab, busy, num, "Mbit/s"); }
    // the other results on one line under the number
    char up[16], dn[16]; fmt_mbit(m.up, up, sizeof up); fmt_mbit(m.down, dn, sizeof dn);
    // shortest last: big numbers don't fit the card (404 px on the Brick) at 28 px with "ms" and the jitter, so those go first
    for (int fit = 0; fit < 3; fit++) {
      int n = 0; s[0] = 0;
      if (m.phase == PH_UP) n += snprintf(s + n, sizeof s - n, "DOWN %s · ", dn);
      if (m.up >= 0) n += snprintf(s + n, sizeof s - n, "UP %s · ", up);
      if (m.ping >= 0) n += snprintf(s + n, sizeof s - n, fit == 0 ? "PING %.0f ms ± %.0f" : fit == 1 ? "PING %.0f ± %.0f" : "PING %.0f",
                                     m.ping, m.jitter);
      if (n >= 3 && !strcmp(s + n - 3, " · ")) s[n - 3] = 0;
      if (dot_text_w(dot, DOT_VALUE, s) <= IN_W) break;
    }
    dot_text(dot, DOT_VALUE, s, 70, 282, DOT_LABEL, DOT_LEFT, IN_W);
    if (busy) {
      double f = m.phase_len > 0 ? (now_ms() - m.phase_t0) / m.phase_len : 0;
      dot_progress(dot, 70, 340, IN_W, f > 1 ? 1 : f, 0);
    } else if (m.low >= 0) {
      Uint32 col; const char *v = verdict(m.low, &col);
      dot_adot(dot, 70, 340, col);
      dot_text(dot, DOT_BODY_S, v, 96, 330, DOT_TEXT, DOT_LEFT, IN_W - 24);
    }
  }
  wifi_card(0, &m);

  // the download, then the upload, two samples a second
  int x0, y0, cols;
  double top = 0;
  for (int i = 0; i < m.ndown; i++) if (m.sdown[i] > top) top = m.sdown[i];
  for (int i = 0; i < m.nup; i++) if (m.sup[i] > top) top = m.sup[i];
  top = nice_top(top > 0 ? top : 1);
  snprintf(s, sizeof s, "TOP %.0f", top);
  graph_panel("Mbit/s · down · up", m.ndown ? s : NULL, &x0, &y0, &cols);
  int c = 0;
  for (int i = 0; i + 1 < m.ndown && c < cols; i += 2, c++) column(x0 + c * G_PITCH, y0, (m.sdown[i] + m.sdown[i + 1]) / 2 / top, DOT_TEXT);
  if (m.nup) c++;
  for (int i = 0; i + 1 < m.nup && c < cols; i += 2, c++) column(x0 + c * G_PITCH, y0, (m.sup[i] + m.sup[i + 1]) / 2 / top, DOT_SERIES_1);
  for (; c < cols; c++) column(x0 + c * G_PITCH, y0, 0, 0);

  if (busy) { DotHint h[] = { { "B", "Stop" } }; dot_dock(dot, h, 1); }
  else {   // Internet: UP/DOWN picks the location (walk is the home server's, so X gives its place up)
    DotHint h[] = { { "A", m.phase == PH_IDLE ? "Test" : "Again" }, { "L1 R1", "Target" },
                    target == T_INTERNET ? (DotHint){ "↑↓", "Location" } : (DotHint){ "X", "Walk" }, { "Y", "History" } };
    dot_dock(dot, h, 4);
  }
}

// --- walk screen --------------------------------------------------------------------------------------------------------
static void draw_walk(void) {
  Meter m; meter_get(&m);
  const Target *t = &targets[T_SERVER];   // walk runs against the home server only (see meter.c)
  char hdr[160], num[16], s[80];
  Uint32 secs = (SDL_GetTicks() - walk_t0) / 1000;
  snprintf(hdr, sizeof hdr, "%s · %u:%02u", t->name, secs / 60, secs % 60);
  dot_header(dot, "SPEED · WALK", hdr);
  if (m.phase == PH_FAIL) {
    big_number("FAILED", DOT_CRITICAL, "—", "Mbit/s");
    dot_text(dot, DOT_BODY_S, m.error, 70, 300, DOT_CRITICAL, DOT_LEFT, IN_W);
  } else {
    fmt_mbit(m.live, num, sizeof num);
    big_number("DOWNLOAD NOW", 1, num, "Mbit/s");
    WifiInfo w; wifi_get(&w);
    double lo = -1; for (int i = m.nwalk > 60 ? m.nwalk - 60 : 0; i < m.nwalk; i++) if (lo < 0 || m.wmbit[i] < lo) lo = m.wmbit[i];
    char l[16]; fmt_mbit(lo, l, sizeof l);
    if (!w.connected) snprintf(s, sizeof s, "NO WI-FI");
    else snprintf(s, sizeof s, "SLOWEST LAST MINUTE %s", l);
    dot_text(dot, DOT_VALUE, s, 70, 282, w.connected ? DOT_LABEL : DOT_CRITICAL, DOT_LEFT, IN_W);
    dot_text(dot, DOT_BODY_S, "Walk: watch where it drops", 70, 330, DOT_LABEL, DOT_LEFT, IN_W);
  }
  wifi_card(1, &m);

  // one column a second, the newest on the right; the signal as a dot over it; a mesh switch lights the top dot
  int x0, y0, cols;
  double top = 0;
  for (int i = 0; i < m.nwalk; i++) if (m.wmbit[i] > top) top = m.wmbit[i];
  top = nice_top(top > 0 ? top : 1);
  snprintf(s, sizeof s, "TOP %.0f", top);
  graph_panel("Mbit/s · signal · switch", s, &x0, &y0, &cols);
  int first = m.nwalk > cols ? m.nwalk - cols : 0;
  for (int c = 0; c < cols; c++) {
    int i = first + c, x = x0 + c * G_PITCH;
    if (i >= m.nwalk) { column(x, y0, 0, 0); continue; }
    column(x, y0, m.wmbit[i] / top, DOT_TEXT);
    if (m.wdbm[i]) {   // -90 dBm at the bottom row, -30 at the top
      double f = (m.wdbm[i] + 90) / 60.0; f = f < 0 ? 0 : f > 1 ? 1 : f;
      int r = (int)(f * (G_ROWS - 1) + .5);
      dot_circle(dot, x, y0 + (G_ROWS - 1 - r) * G_VPITCH + 5, 5, DOT_SERIES_1);
    }
    if (m.wroam[i]) dot_circle(dot, x, y0 + 5, 5, DOT_WARN);   // a mesh switch: the top dot
  }
  DotHint h[] = { { "B", "Stop" } };
  dot_dock(dot, h, 1);
}

// --- history ------------------------------------------------------------------------------------------------------------
#define HIST_PITCH (DOT_ROW_H + 12)
#define HIST_ROWS ((DOT_CONTENT_BOTTOM - 96 + 12) / HIST_PITCH)   // Brick 5 (to 624), Smart Pro 4 (to 516): clear of the dock
static void draw_history(void) {
  char cnt[32]; int n = history_count();
  snprintf(cnt, sizeof cnt, "%d run%s", n, n == 1 ? "" : "s");
  dot_header(dot, "SPEED · HISTORY", cnt);
  if (!n) dot_text(dot, DOT_TITLE, "No runs yet", 70, 300, DOT_ON_GROUND, DOT_LEFT, 0);
  if (hist_sel < hist_top) hist_top = hist_sel;
  if (hist_sel >= hist_top + HIST_ROWS) hist_top = hist_sel - HIST_ROWS + 1;
  for (int k = 0; k < HIST_ROWS && hist_top + k < n; k++) {
    const Run *r = history_get(hist_top + k);
    char name[80], sub[160], meta[32], dn[16], up[16], ap[24];
    fmt_mbit(r->down, dn, sizeof dn); fmt_mbit(r->up, up, sizeof up); fmt_ap(r->bssid, ap, sizeof ap);
    if (r->up >= 0) snprintf(name, sizeof name, "↓ %s  ↑ %s Mbit/s", dn, up);
    else snprintf(name, sizeof name, "↓ %s Mbit/s", dn);
    snprintf(sub, sizeof sub, "%s · ping %.0f ms · %d dBm · ch %d · %s", r->target < NTARGETS ? targets[r->target].name : "?",
             r->ping, r->dbm, wifi_channel(r->freq), ap);
    time_t tt = r->when; struct tm tm; localtime_r(&tt, &tm);
    strftime(meta, sizeof meta, "%d %b %H:%M", &tm);
    Uint32 col; verdict(r->low, &col);
    dot_row(dot, DOT_MARGIN, 96 + k * HIST_PITCH, DOT_CONTENT_W, DOT_I_NONE, name, sub, meta, hist_top + k == hist_sel ? DOT_FOCUS : 0, 0);
    (void)col;
  }
  DotHint h[] = { { "B", "Back" } };
  dot_dock(dot, h, 1);
}

static void draw(void) {
  dot_frame(dot);
  dot_ground(dot);
  if (screen == SCR_TEST) draw_test();
  else if (screen == SCR_WALK) draw_walk();
  else draw_history();
  Uint32 now = SDL_GetTicks();
  if (now < toast_until) { Uint32 l = toast_until - now; dot_toast(dot, toast, l < DOT_FADE_MS ? l / (float)DOT_FADE_MS : 1); }
}

// --- input --------------------------------------------------------------------------------------------------------------
static void on_button(int b) {
  Meter m; meter_get(&m);
  int busy = m.phase == PH_PING || m.phase == PH_DOWN || m.phase == PH_UP;
  if (b == BTN_MENU) { running = 0; return; }
  if (screen == SCR_HISTORY) {
    if (b == BTN_B || b == BTN_Y) screen = SCR_TEST;
    else if (b == BTN_UP && hist_sel > 0) hist_sel--;
    else if (b == BTN_DOWN && hist_sel + 1 < history_count()) hist_sel++;
    return;
  }
  if (screen == SCR_WALK) {
    if (b == BTN_B || b == BTN_X) { meter_stop(); screen = SCR_TEST; }
    return;
  }
  if (busy) { if (b == BTN_B) meter_stop(); return; }
  switch (b) {
  case BTN_A:
    if (!targets[target].ok) { show_toast(targets[target].detail); break; }
    meter_start(target); break;
  case BTN_X:   // minutes of downloading: the home server's job, not volunteers' LibreSpeed servers'
    if (!targets[T_SERVER].ok) { show_toast("Walk needs the Home server: set server= in speed.txt"); break; }
    meter_walk(T_SERVER); walk_t0 = SDL_GetTicks(); screen = SCR_WALK; break;
  case BTN_Y: hist_sel = hist_top = 0; screen = SCR_HISTORY; break;
  case BTN_UP: case BTN_DOWN: {   // Internet's location: Auto, then the servers nearest first
    if (target != T_INTERNET) break;
    int n = meter_location_count();
    if (!n) { show_toast("Finding LibreSpeed's servers…"); break; }
    int i = meter_location() + (b == BTN_DOWN ? 1 : -1);
    if (i < -1) i = n - 1; else if (i >= n) i = -1;
    meter_set_location(i);
    char name[64], msg[96]; double ms; meter_location_name(i, name, sizeof name, &ms);
    if (i < 0) snprintf(msg, sizeof msg, "Auto: the quickest server");
    else if (ms >= 0) snprintf(msg, sizeof msg, "%s · %.0f ms", name, ms);
    else snprintf(msg, sizeof msg, "%s · no answer at start", name);
    show_toast(msg);
    { Meter z; meter_get(&z); z.phase = PH_IDLE; meter_fake(&z); }
    break;
  }
  case BTN_L1: case BTN_R1: case BTN_LEFT: case BTN_RIGHT:
    meter_stop();
    target = (target + (b == BTN_L1 || b == BTN_LEFT ? NTARGETS - 1 : 1)) % NTARGETS;
    { Meter z; meter_get(&z); z.phase = PH_IDLE; meter_fake(&z); }
    break;
  case BTN_B: running = 0; break;
  }
}

// --- shots --------------------------------------------------------------------------------------------------------------
static void shot(const char *dir, const char *name) {
  draw();
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, DOT_W, DOT_H, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
  char p[700]; snprintf(p, sizeof p, "%s/%s.bmp", dir, name);
  SDL_SaveBMP(s, p); SDL_FreeSurface(s);
  printf("%s\n", p);
}

static void shots(const char *out) {
  WifiInfo w = { .connected = 1, .dbm = -49, .freq = 2427, .width = 40, .rate = 121.5, .roams = 2 };
  snprintf(w.ssid, sizeof w.ssid, "Home"); snprintf(w.bssid, sizeof w.bssid, "02:00:00:00:a1:1a");
  snprintf(w.gateway, sizeof w.gateway, "192.168.1.1");
  wifi_fake(&w);
  targets[T_SERVER].ok = 1; snprintf(targets[T_SERVER].detail, sizeof targets[T_SERVER].detail, "192.168.1.20");
  snprintf(targets[T_INTERNET].detail, sizeof targets[T_INTERNET].detail, "LibreSpeed · Amsterdam");
  Meter m = { 0 }; m.ping = m.jitter = m.router = m.down = m.up = m.low = -1;
  meter_fake(&m); shot(out, "test-ready");
  // mid-download: a Wi-Fi with dips
  srand(7);
  m.phase = PH_DOWN; m.ping = 14; m.jitter = 3; m.router = 3; m.phase_t0 = now_ms() - 6200; m.phase_len = 10000;
  for (int i = 0; i < 25; i++) m.sdown[i] = i < 3 ? 4 + i * 4 : 15 + (rand() % 60) / 10.0 - (i % 9 == 0 ? 9 : 0);
  m.ndown = 25; m.live = 16.4;
  meter_fake(&m); shot(out, "test-download");
  for (int i = 25; i < 40; i++) m.sdown[i] = 15 + (rand() % 60) / 10.0 - (i % 9 == 0 ? 9 : 0);
  m.ndown = 40;
  for (int i = 0; i < 32; i++) m.sup[i] = i < 2 ? 2 + i * 2 : 5 + (rand() % 20) / 10.0;
  m.nup = 32; m.down = 17.0; m.up = 5.6; m.low = 7.1; m.phase = PH_DONE;
  meter_fake(&m); shot(out, "test-done");
  m.low = 2.4; m.down = 3.9; meter_fake(&m); shot(out, "test-done-slow");
  Meter f = { 0 }; f.phase = PH_FAIL; f.ping = f.jitter = f.router = f.down = f.up = f.low = -1;
  snprintf(f.error, sizeof f.error, "Can't reach the internet"); meter_fake(&f); shot(out, "test-failed");
  // walk: the speed falls going away from the router, a switch to the other mesh point, then it recovers
  Meter wk = { 0 }; wk.phase = PH_WALK; wk.ping = wk.jitter = wk.router = wk.down = wk.up = wk.low = -1;
  for (int i = 0; i < 70; i++) {
    double d = i < 30 ? 18 - i * 0.5 : i < 38 ? 3 + (i - 30) * 0.2 : 14 + (rand() % 40) / 10.0;
    wk.wmbit[i] = d; wk.wdbm[i] = i < 30 ? -48 - i : i < 38 ? -79 : -55; wk.wroam[i] = i == 38;
  }
  wk.nwalk = 70; wk.live = 15.2;
  meter_fake(&wk); screen = SCR_WALK; walk_t0 = SDL_GetTicks() - 70000; shot(out, "walk");
  meter_fake(&m);
  screen = SCR_HISTORY;
  for (int i = 7; i >= 0; i--) {
    Run r = { .when = 1795000000 - i * 5400, .target = i % 3 == 2 ? T_SERVER : T_INTERNET, .down = 17.0 - i * 1.7,
              .up = 5.6 - i * 0.3, .ping = 14 + i, .jitter = 3, .router = 3, .low = 7 - i * 0.8,
              .dbm = -49 - i * 3, .freq = 2427 };
    snprintf(r.bssid, sizeof r.bssid, i % 2 ? "02:00:00:00:b2:f6" : "02:00:00:00:a1:1a");
    history_add(&r);
  }
  hist_sel = 1; shot(out, "history");
}

// --cli [internet|server]: one test without a screen, results on stdout (over SSH, or in the build container)
static int cli(const char *name) {
  int t = !name || !strcmp(name, "internet") ? T_INTERNET : T_SERVER;
  if (!targets[t].ok) { printf("%s: %s\n", targets[t].name, targets[t].detail); return 1; }
  printf("%s · %s\n", targets[t].name, targets[t].detail);
  meter_start(t);
  Meter m; int last = -1;
  do {
    SDL_Delay(500); meter_get(&m);
    if (m.phase != last) { static const char *n[] = { "idle", "ping", "download", "upload", "done", "failed", "walk" };
                           printf("  %s\n", n[m.phase]); last = m.phase; }
    else if (m.phase == PH_DOWN || m.phase == PH_UP) printf("    %.1f Mbit/s\n", m.live);
  } while (m.phase != PH_DONE && m.phase != PH_FAIL);
  if (m.phase == PH_FAIL) { printf("failed: %s\n", m.error); return 1; }
  printf("down %.1f Mbit/s (slowest second %.1f) · up %.1f · ping %.0f ms ± %.0f · router %.0f ms\n",
         m.down, m.low, m.up, m.ping, m.jitter, m.router);
  return 0;
}

int main(int argc, char **argv) {
  const char *user = argc > 1 ? argv[1] : "/tmp/speed", *pak = argc > 2 ? argv[2] : ".";
  const char *shots_dir = argc > 4 && !strcmp(argv[3], "--shots") ? argv[4] : NULL;
  if (argc > 3 && !strcmp(argv[3], "--cli")) {   // no window: SDL only for threads and timers
    SDL_Init(SDL_INIT_TIMER);
    char ca[700]; snprintf(ca, sizeof ca, "%s/cacert.pem", pak);
    if (!net_init(ca)) fprintf(stderr, "HTTPS won't work: %s\n", ca);
    history_open(user); meter_targets(user); wifi_start();
    return cli(argc > 4 ? argv[4] : NULL);
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  dot_screen_detect(); graph_fit();
  SDL_Window *win = SDL_CreateWindow("Speed", 0, 0, DOT_W, DOT_H, shots_dir ? 0 : SDL_WINDOW_FULLSCREEN);
  ren = SDL_CreateRenderer(win, -1, shots_dir ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
  SDL_RenderSetLogicalSize(ren, DOT_W, DOT_H);
  for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  char p[700]; snprintf(p, sizeof p, "%s/fonts", pak);
  if (!(dot = dot_init(ren, p, DOT_ACCENT_SPEED))) { fprintf(stderr, "fonts in %s: %s\n", p, SDL_GetError()); return 1; }
  { static const DotTheme th = DOT_THEME_SPEED; dot_set_theme(dot, &th); }   // violet evening ground, violet state dot
  snprintf(p, sizeof p, "%s/cacert.pem", pak);
  if (!net_init(p)) fprintf(stderr, "HTTPS won't work: %s\n", p);
  history_open(user);
  meter_targets(user);
  if (shots_dir) { shots(shots_dir); dot_quit(dot); SDL_Quit(); return 0; }
  wifi_start();
  meter_locations_load();
  if (!targets[target].ok) target = T_INTERNET;

  while (running) {
    SDL_Event e;
    int got = SDL_WaitEventTimeout(&e, 50);
    while (got) {
      if (e.type == SDL_QUIT) running = 0;
      else if (e.type == SDL_CONTROLLERBUTTONDOWN) on_button(e.cbutton.button);
      got = SDL_PollEvent(&e);
    }
    draw();
    SDL_RenderPresent(ren);
  }
  meter_stop();
  dot_quit(dot);
  SDL_Quit();
  return 0;
}
