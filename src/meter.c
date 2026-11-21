// The tests. A test thread runs the phases (ping, download, upload) and counts bytes from worker threads, one per
// connection, four at a time (one TCP stream rarely fills a Wi-Fi link). Internet is LibreSpeed (open source): of its
// public servers, the one that answers fastest, with garbage.php to download from and empty.php to upload to. Walk
// (home server only: minutes of downloading is no job for volunteers' servers) keeps two downloads going and samples
// the Wi-Fi once a second. The UI reads a copy of the state (meter_get) every frame.
#include "speed.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

Target targets[NTARGETS];

#define STREAMS 4
#define DOWN_MS 10000
#define UP_MS 8000
#define UP_BYTES (25LL * 1000 * 1000)
#define LS_UP_BYTES (4LL * 1000 * 1000)   // LibreSpeed servers cap uploads (Clouvider: 413 at 20 MB); 4 MB, looped
#define LS_SERVERS "https://librespeed.org/backend-servers/servers.php"
#define LS_MAX 64

static SDL_mutex *mx;
static Meter M;
static SDL_Thread *th;
static volatile int stopping;
static long long total;           // bytes moved by the workers (__atomic)
static Conn *live_conns[STREAMS];  // so stopping can cut them off
static int active;                 // workers still running (a phase ends early when all have stopped)
// LibreSpeed's servers (see ls_load)
typedef struct { char name[96], base[300], dl[80], ul[80], label[64]; double ms; } LsServer;
static LsServer sv[LS_MAX];
static int ls_n, ls_loaded, ls_sel = -1;   // ls_sel: -1 Auto, else an index into sv
static char ls_want[96], ls_file[700];     // the chosen server's name (from location.txt until the list is in)
static SDL_mutex *ls_mx;

static void add_bytes(long long n) { __atomic_add_fetch(&total, n, __ATOMIC_RELAXED); }
static long long get_bytes(void) { return __atomic_load_n(&total, __ATOMIC_RELAXED); }

static int set_phase(int ph, double len) {   // 0 if the test was stopped meanwhile (meter_stop set PH_IDLE)
  SDL_LockMutex(mx);
  int go = M.phase != PH_IDLE;
  if (go) { M.phase = ph; M.phase_t0 = now_ms(); M.phase_len = len; M.live = 0; }
  SDL_UnlockMutex(mx);
  return go;
}
static void fail(const char *why) {
  SDL_LockMutex(mx); if (M.phase == PH_IDLE) { SDL_UnlockMutex(mx); return; }
  M.phase = PH_FAIL; snprintf(M.error, sizeof M.error, "%s", why); SDL_UnlockMutex(mx);
}

// --- targets --------------------------------------------------------------------------------------------------------
static int read_kv(const char *path, const char *key, char *out, int n) {
  FILE *f = fopen(path, "r"); if (!f) return 0;
  char line[600]; int klen = strlen(key), found = 0;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, key, klen) && line[klen] == '=') {
      snprintf(out, n, "%s", line + klen + 1); out[strcspn(out, "\r\n")] = 0; found = 1;
    }
  fclose(f);
  return found;
}

static void host_of(const char *url, char *out, int n) {
  const char *p = strstr(url, "://"); p = p ? p + 3 : url;
  snprintf(out, n, "%.*s", (int)strcspn(p, ":/"), p);
}

void meter_targets(const char *userdata) {
  if (!mx) mx = SDL_CreateMutex();
  Target *t = &targets[T_INTERNET];
  t->kind = T_INTERNET; t->ok = 1;
  snprintf(t->name, sizeof t->name, "Internet");   // URLs: ls_pick()
  if (!ls_mx) ls_mx = SDL_CreateMutex();
  snprintf(ls_file, sizeof ls_file, "%s/location.txt", userdata);
  FILE *lf = fopen(ls_file, "r");
  if (lf) { if (fgets(ls_want, sizeof ls_want, lf)) ls_want[strcspn(ls_want, "\r\n")] = 0; fclose(lf); }
  snprintf(t->detail, sizeof t->detail, "LibreSpeed · %s", ls_want[0] ? "…" : "Auto");

  // speed-server.py on a PC: server=http://<ip>:<port> in <userdata>/speed.txt
  t = &targets[T_SERVER]; t->kind = T_SERVER;
  snprintf(t->name, sizeof t->name, "Home server");
  char cfg[700], srv[300]; snprintf(cfg, sizeof cfg, "%s/speed.txt", userdata);
  if (read_kv(cfg, "server", srv, sizeof srv) && srv[0]) {
    t->ok = 1; srv[strcspn(srv, " ")] = 0;
    if (srv[strlen(srv) - 1] == '/') srv[strlen(srv) - 1] = 0;
    host_of(srv, t->detail, sizeof t->detail);
    snprintf(t->down, sizeof t->down, "%s/down?bytes=100000000", srv);
    snprintf(t->up, sizeof t->up, "%s/up", srv);
    snprintf(t->ping, sizeof t->ping, "%s/", srv);
  } else snprintf(t->detail, sizeof t->detail, "set server= in speed.txt");
}

// --- workers --------------------------------------------------------------------------------------------------------
typedef struct { const Target *t; int slot, up; } Job;

static void hold(int slot, Conn *c) { SDL_LockMutex(mx); live_conns[slot] = c; SDL_UnlockMutex(mx); }

static int worker(void *ud) {
  Job *j = ud;
  static char zeros[16384];
  char *buf = malloc(65536);
  while (!stopping) {
    long long up_bytes = j->t->kind == T_INTERNET ? LS_UP_BYTES : UP_BYTES;
    Conn *c = conn_request(j->up ? "POST" : "GET", j->up ? j->t->up : j->t->down, j->t->headers, j->up ? up_bytes : 0);
    if (!c) { SDL_Delay(300); continue; }
    hold(j->slot, c);
    if (j->up) {
      for (long long left = up_bytes; left > 0 && !stopping; ) {
        int n = left < (long long)sizeof zeros ? (int)left : (int)sizeof zeros;
        if (conn_write(c, zeros, n) != n) break;
        add_bytes(n); left -= n;
      }
    } else if (conn_head(c) == 200) {
      int r;
      while (!stopping && (r = conn_read(c, buf, 65536)) > 0) add_bytes(r);
    } else SDL_Delay(300);
    hold(j->slot, NULL);
    conn_close(c);
  }
  free(buf);
  __atomic_sub_fetch(&active, 1, __ATOMIC_RELAXED);
  return 0;
}

static void cut_all(void) {   // stop the workers now, not after their current read
  stopping = 1;
  SDL_LockMutex(mx);
  for (int i = 0; i < STREAMS; i++) conn_abort(live_conns[i]);
  SDL_UnlockMutex(mx);
}

// Runs n workers for ms milliseconds, a sample every 1/SAMPLES_PER_S s into s[]; returns Mbit/s after the first second
// (TCP's slow start) and the slowest second in *low.
static double transfer(const Target *t, int up, int n, double ms, float *s, int *ns, double *low) {
  SDL_Thread *w[STREAMS]; Job jobs[STREAMS];
  stopping = 0; __atomic_store_n(&total, 0, __ATOMIC_RELAXED); __atomic_store_n(&active, n, __ATOMIC_RELAXED);
  for (int i = 0; i < n; i++) { jobs[i] = (Job){ t, i, up }; w[i] = SDL_CreateThread(worker, "worker", &jobs[i]); }
  double t0 = now_ms(), last_t = t0, warm_t = 0; long long last_b = 0, warm_b = 0;
  int k = 0;
  while (now_ms() - t0 < ms && !(M.phase == PH_IDLE) && __atomic_load_n(&active, __ATOMIC_RELAXED) > 0) {
    SDL_Delay(1000 / SAMPLES_PER_S);
    double tn = now_ms(); long long b = get_bytes();
    float mbit = (b - last_b) * 8 / ((tn - last_t) * 1000);
    SDL_LockMutex(mx);
    if (k < MAX_SAMPLES) { s[k++] = mbit; *ns = k; }
    M.live = k >= 2 ? (s[k - 1] + s[k - 2]) / 2 : mbit;
    SDL_UnlockMutex(mx);
    last_b = b; last_t = tn;
    if (!warm_t && tn - t0 >= 1000) { warm_t = tn; warm_b = b; }
  }
  double tn = now_ms(); long long b = get_bytes();
  cut_all();
  for (int i = 0; i < n; i++) SDL_WaitThread(w[i], NULL);
  *low = -1;
  for (int i = SAMPLES_PER_S; i + SAMPLES_PER_S <= k; i++) {   // one-second windows after the first second
    double a = 0; for (int j = 0; j < SAMPLES_PER_S; j++) a += s[i + j];
    a /= SAMPLES_PER_S; if (*low < 0 || a < *low) *low = a;
  }
  if (!warm_t || tn - warm_t < 500) return b > 0 ? b * 8 / ((tn - t0) * 1000) : 0;
  return (b - warm_b) * 8 / ((tn - warm_t) * 1000);
}

// --- ping -----------------------------------------------------------------------------------------------------------
static unsigned short cksum(void *p, int n) {
  unsigned sum = 0; unsigned short *w = p;
  for (; n > 1; n -= 2) sum += *w++;
  if (n) sum += *(unsigned char *)w;
  sum = (sum >> 16) + (sum & 0xffff); sum += sum >> 16;
  return ~sum;
}

// ICMP echo to ip, n times (the Brick runs paks as root, so a raw socket works); fills ms[], returns how many answered
static int icmp_rtt(const char *ip, double *ms, int n) {
  struct sockaddr_in to = { .sin_family = AF_INET };
  if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) return 0;
  int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (fd < 0) return 0;
  struct timeval tv = { 1, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  int ok = 0, id = getpid() & 0xffff;
  for (int i = 0; i < n; i++) {
    struct icmphdr h = { .type = ICMP_ECHO }; h.un.echo.id = htons(id); h.un.echo.sequence = htons(i);
    h.checksum = cksum(&h, sizeof h);
    double t0 = now_ms();
    if (sendto(fd, &h, sizeof h, 0, (struct sockaddr *)&to, sizeof to) < 0) break;
    for (;;) {   // skip other replies (another program's pings)
      unsigned char b[128]; int r = recv(fd, b, sizeof b, 0);
      if (r < 0) break;
      struct iphdr *iph = (struct iphdr *)b; int hl = iph->ihl * 4;
      struct icmphdr *rh = (struct icmphdr *)(b + hl);
      if (r >= hl + 8 && rh->type == ICMP_ECHOREPLY && ntohs(rh->un.echo.id) == id && ntohs(rh->un.echo.sequence) == i) {
        ms[ok++] = now_ms() - t0; break;
      }
    }
    usleep(150000);
  }
  close(fd);
  return ok;
}

static int cmpd(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return (x > y) - (x < y); }
// median, and jitter = the mean change from one round trip to the next (in the order measured)
static double median(double *v, int n, double *jitter) {
  if (n <= 0) { if (jitter) *jitter = -1; return -1; }
  if (jitter) { double j = 0; for (int i = 1; i < n; i++) j += v[i] > v[i - 1] ? v[i] - v[i - 1] : v[i - 1] - v[i];
                *jitter = n > 1 ? j / (n - 1) : 0; }
  qsort(v, n, sizeof *v, cmpd);
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// --- the test -------------------------------------------------------------------------------------------------------
// LibreSpeed: its public server list, a TCP connect timed to each (all at once), sorted nearest first. Loaded once,
// in the background at start (meter_locations_load) or by the first test. The location is Auto (the quickest that
// really serves data: volunteers' servers come and go, some answer 503) or one chosen with UP/DOWN, kept by name in
// <userdata>/location.txt.

static void ls_join(char *out, int n, const char *base, const char *path) {
  int slash = base[0] && base[strlen(base) - 1] == '/';
  snprintf(out, n, "%s%s%s", base, slash || path[0] == '/' ? "" : "/", path);
}
static int ls_probe(void *ud) {
  LsServer *s = ud; double v[2]; int n = net_rtt(s->base, v, 2);
  s->ms = n ? (n == 2 && v[1] < v[0] ? v[1] : v[0]) : -1;
  return 0;
}
static int ls_cmp(const void *a, const void *b) {
  double x = ((const LsServer *)a)->ms, y = ((const LsServer *)b)->ms;
  if (x < 0) return y < 0 ? 0 : 1;
  if (y < 0) return -1;
  return (x > y) - (x < y);
}
// "Amsterdam, Netherlands (Clouvider)" -> "Amsterdam · Clouvider"
static void ls_label(const char *name, char *out, int n) {
  int city = (int)strcspn(name, ",(");
  while (city > 0 && name[city - 1] == ' ') city--;
  const char *open = strrchr(name, '('), *close = open ? strchr(open, ')') : NULL;
  if (open && close && close > open + 1) snprintf(out, n, "%.*s · %.*s", city, name, (int)(close - open - 1), open + 1);
  else snprintf(out, n, "%.*s", city, name);
}
static void ls_detail(Target *t, const char *what) {
  SDL_LockMutex(mx); snprintf(t->detail, sizeof t->detail, "LibreSpeed · %s", what); SDL_UnlockMutex(mx);
}
static int ls_load(void) {
  SDL_LockMutex(ls_mx);
  if (ls_loaded) { SDL_UnlockMutex(ls_mx); return ls_n; }
  static char buf[65536]; static LsServer tmp[LS_MAX]; int n = 0;
  if (http_get(LS_SERVERS, "Accept: application/json\r\n", buf, sizeof buf) > 0) {
    cJSON *j = cJSON_Parse(buf), *it;
    cJSON_ArrayForEach(it, j) {
      const char *base = cJSON_GetStringValue(cJSON_GetObjectItem(it, "server"));
      const char *dl = cJSON_GetStringValue(cJSON_GetObjectItem(it, "dlURL")), *ul = cJSON_GetStringValue(cJSON_GetObjectItem(it, "ulURL"));
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
      if (!base || !dl || !ul || !name || strncmp(base, "http", 4) || n == LS_MAX) continue;
      LsServer *s = &tmp[n++];
      snprintf(s->name, sizeof s->name, "%s", name); snprintf(s->base, sizeof s->base, "%s", base);
      snprintf(s->dl, sizeof s->dl, "%s", dl); snprintf(s->ul, sizeof s->ul, "%s", ul);
      ls_label(name, s->label, sizeof s->label);
    }
    cJSON_Delete(j);
    SDL_Thread *th[LS_MAX];
    for (int i = 0; i < n; i++) th[i] = SDL_CreateThread(ls_probe, "probe", &tmp[i]);
    for (int i = 0; i < n; i++) SDL_WaitThread(th[i], NULL);
    qsort(tmp, n, sizeof *tmp, ls_cmp);
  }
  SDL_LockMutex(mx);
  memcpy(sv, tmp, sizeof *sv * n); ls_n = n; ls_loaded = n > 0;   // a failed load is tried again by the next test
  ls_sel = -1;
  for (int i = 0; i < n && ls_want[0]; i++) if (!strcmp(sv[i].name, ls_want)) ls_sel = i;
  SDL_UnlockMutex(mx);
  SDL_UnlockMutex(ls_mx);
  return n;
}
static int ls_load_thread(void *ud) { (void)ud; ls_load(); return 0; }
void meter_locations_load(void) { SDL_DetachThread(SDL_CreateThread(ls_load_thread, "servers", NULL)); }

int meter_location_count(void) { SDL_LockMutex(mx); int n = ls_loaded ? ls_n : 0; SDL_UnlockMutex(mx); return n; }
int meter_location(void) { return ls_sel; }
// out: "Auto" or "Frankfurt · Clouvider", plus the round trip measured at start (ms, -1 unknown)
void meter_location_name(int i, char *out, int n, double *ms) {
  SDL_LockMutex(mx);
  if (i < 0 || i >= ls_n) { snprintf(out, n, "Auto"); if (ms) *ms = -1; }
  else { snprintf(out, n, "%s", sv[i].label); if (ms) *ms = sv[i].ms; }
  SDL_UnlockMutex(mx);
}
void meter_set_location(int i) {
  SDL_LockMutex(mx);
  ls_sel = i >= 0 && i < ls_n ? i : -1;
  snprintf(ls_want, sizeof ls_want, "%s", ls_sel >= 0 ? sv[ls_sel].name : "");
  snprintf(targets[T_INTERNET].detail, sizeof targets[T_INTERNET].detail, "LibreSpeed · %s", ls_sel >= 0 ? sv[ls_sel].label : "Auto");
  SDL_UnlockMutex(mx);
  FILE *f = fopen(ls_file, "w"); if (f) { fprintf(f, "%s\n", ls_want); fclose(f); }
}

// The server for this test into the Internet target's URLs: the chosen one, or (Auto) the nearest that serves data.
static int ls_pick(Target *t, char *why, int wn) {
  if (!ls_load()) { snprintf(why, wn, "Can't get LibreSpeed's server list"); return 0; }
  int sel = ls_sel;
  for (int i = sel >= 0 ? sel : 0; i < ls_n && (sel >= 0 ? i == sel : i < 5); i++) {
    if (sv[i].ms < 0 && sel < 0) break;
    char url[600]; ls_join(url, sizeof url, sv[i].base, sv[i].dl);
    char probe[640]; snprintf(probe, sizeof probe, "%s?ckSize=1", url);
    Conn *c = conn_request("GET", probe, NULL, 0);
    int st = c ? conn_head(c) : -2;   // -2: no connection, -1: no answer
    conn_close(c);
    fprintf(stderr, "librespeed: %s %.0f ms -> %d\n", sv[i].label, sv[i].ms, st);
    if (st != 200) {
      if (sel >= 0) { snprintf(why, wn, st > 0 ? "%s is down (%d)" : "%s didn't answer", sv[i].label, st); return 0; }
      continue;
    }
    SDL_LockMutex(mx);
    snprintf(t->down, sizeof t->down, "%s?ckSize=100", url);
    ls_join(t->up, sizeof t->up, sv[i].base, sv[i].ul);
    snprintf(t->ping, sizeof t->ping, "%s", sv[i].base);
    SDL_UnlockMutex(mx);
    ls_detail(t, sv[i].label);
    return 1;
  }
  snprintf(why, wn, "No LibreSpeed server answered");
  return 0;
}

static int test_thread(void *ud) {
  Target *t = &targets[(int)(intptr_t)ud];
  WifiInfo w; wifi_get(&w);
  if (!set_phase(PH_PING, 3000)) return 0;
  double v[10]; int n;
  if (w.gateway[0] && (n = icmp_rtt(w.gateway, v, 8)) > 0) { double r = median(v, n, NULL); SDL_LockMutex(mx); M.router = r; SDL_UnlockMutex(mx); }
  if (t->kind == T_INTERNET) { char why[96]; if (!ls_pick(t, why, sizeof why)) { fail(why); return 0; } }
  if ((n = net_rtt(t->ping, v, 8)) == 0) { fail(t->kind == T_INTERNET ? "Can't reach the internet" : "Can't reach the server"); return 0; }
  double jit, p = median(v, n, &jit);
  SDL_LockMutex(mx); M.ping = p; M.jitter = jit; SDL_UnlockMutex(mx);
  if (!set_phase(PH_DOWN, DOWN_MS)) return 0;
  double low, d = transfer(t, 0, STREAMS, DOWN_MS, M.sdown, &M.ndown, &low);
  SDL_LockMutex(mx); M.down = d; M.low = low; SDL_UnlockMutex(mx);
  if (M.phase == PH_IDLE) return 0;
  if (d <= 0) { fail("Nothing came through"); return 0; }

  if (t->up[0]) {
    if (!set_phase(PH_UP, UP_MS)) return 0;
    double lo2, u = transfer(t, 1, STREAMS, UP_MS, M.sup, &M.nup, &lo2);
    SDL_LockMutex(mx); M.up = u; SDL_UnlockMutex(mx);
    if (M.phase == PH_IDLE) return 0;
  }
  wifi_get(&w);
  Run r = { .when = time(NULL), .target = t->kind, .down = M.down, .up = M.up, .ping = M.ping, .jitter = M.jitter,
            .router = M.router, .low = M.low, .dbm = w.dbm, .freq = w.freq };
  snprintf(r.bssid, sizeof r.bssid, "%s", w.bssid);
  history_add(&r);
  if (!set_phase(PH_DONE, 0)) return 0;
  return 0;
}

// Walk: two downloads that never end, and once a second: Mbit/s, signal, and whether the access point changed
static int walk_thread(void *ud) {
  Target *t = &targets[(int)(intptr_t)ud];
  SDL_Thread *wk[2]; Job jobs[2];
  stopping = 0; __atomic_store_n(&total, 0, __ATOMIC_RELAXED); __atomic_store_n(&active, 2, __ATOMIC_RELAXED);
  for (int i = 0; i < 2; i++) { jobs[i] = (Job){ t, i, 0 }; wk[i] = SDL_CreateThread(worker, "walker", &jobs[i]); }
  long long last_b = 0; double last_t = now_ms(); int roams = -1;
  while (M.phase == PH_WALK) {
    for (int i = 0; i < 4 && M.phase == PH_WALK; i++) {   // the live number every quarter second
      SDL_Delay(250);
      double tn = now_ms(); long long b = get_bytes();
      SDL_LockMutex(mx); M.live = (b - last_b) * 8 / ((tn - last_t) * 1000); SDL_UnlockMutex(mx);
      if (i == 3) {
        WifiInfo w; wifi_get(&w);
        SDL_LockMutex(mx);
        if (M.nwalk == MAX_SAMPLES) {
          memmove(M.wmbit, M.wmbit + 1, sizeof *M.wmbit * (MAX_SAMPLES - 1));
          memmove(M.wdbm, M.wdbm + 1, MAX_SAMPLES - 1); memmove(M.wroam, M.wroam + 1, MAX_SAMPLES - 1);
          M.nwalk--;
        }
        M.wmbit[M.nwalk] = M.live; M.wdbm[M.nwalk] = w.connected ? w.dbm : 0;
        M.wroam[M.nwalk] = roams >= 0 && w.roams != roams; M.nwalk++;
        SDL_UnlockMutex(mx);
        roams = w.roams; last_b = b; last_t = tn;
      }
    }
  }
  cut_all();
  for (int i = 0; i < 2; i++) SDL_WaitThread(wk[i], NULL);
  return 0;
}

static void begin(int phase) {
  meter_stop();
  SDL_LockMutex(mx);
  memset(&M, 0, sizeof M);
  M.ping = M.jitter = M.router = M.down = M.up = M.low = -1;
  M.phase = phase; M.phase_t0 = now_ms();
  SDL_UnlockMutex(mx);
}

void meter_start(int target) { begin(PH_PING); th = SDL_CreateThread(test_thread, "test", (void *)(intptr_t)target); }
void meter_walk(int target) { begin(PH_WALK); th = SDL_CreateThread(walk_thread, "walk", (void *)(intptr_t)target); }

void meter_stop(void) {   // back to idle; the threads see it and end (their connections cut off)
  if (!th) return;
  SDL_LockMutex(mx); int ph = M.phase; M.phase = PH_IDLE; SDL_UnlockMutex(mx);
  cut_all();
  SDL_WaitThread(th, NULL); th = NULL;
  if (ph == PH_DONE || ph == PH_FAIL) { SDL_LockMutex(mx); M.phase = ph; SDL_UnlockMutex(mx); }   // results stay
}

void meter_get(Meter *m) { SDL_LockMutex(mx); *m = M; SDL_UnlockMutex(mx); }
void meter_fake(const Meter *m) { SDL_LockMutex(mx); M = *m; SDL_UnlockMutex(mx); }
