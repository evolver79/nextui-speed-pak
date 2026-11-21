// The tests. A test thread runs the phases (ping, download, upload) and counts bytes from worker threads, one per
// connection, four at a time (one TCP stream rarely fills a Wi-Fi link). Walk keeps two downloads going and samples
// the Wi-Fi once a second. The UI reads a copy of the state (meter_get) every frame.
#include "speed.h"
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

static SDL_mutex *mx;
static Meter M;
static SDL_Thread *th;
static volatile int stopping;
static long long total;           // bytes moved by the workers (__atomic)
static Conn *live_conns[STREAMS];  // so stopping can cut them off

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
  snprintf(t->name, sizeof t->name, "Internet"); snprintf(t->detail, sizeof t->detail, "Cloudflare");
  snprintf(t->down, sizeof t->down, "https://speed.cloudflare.com/__down?bytes=25000000");
  snprintf(t->up, sizeof t->up, "https://speed.cloudflare.com/__up");
  snprintf(t->ping, sizeof t->ping, "https://speed.cloudflare.com/");

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
    Conn *c = conn_request(j->up ? "POST" : "GET", j->up ? j->t->up : j->t->down, j->t->headers, j->up ? UP_BYTES : 0);
    if (!c) { SDL_Delay(300); continue; }
    hold(j->slot, c);
    if (j->up) {
      for (long long left = UP_BYTES; left > 0 && !stopping; ) {
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
  stopping = 0; __atomic_store_n(&total, 0, __ATOMIC_RELAXED);
  for (int i = 0; i < n; i++) { jobs[i] = (Job){ t, i, up }; w[i] = SDL_CreateThread(worker, "worker", &jobs[i]); }
  double t0 = now_ms(), last_t = t0, warm_t = 0; long long last_b = 0, warm_b = 0;
  int k = 0;
  while (now_ms() - t0 < ms && !(M.phase == PH_IDLE)) {
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
static int test_thread(void *ud) {
  Target *t = &targets[(int)(intptr_t)ud];
  WifiInfo w; wifi_get(&w);
  if (!set_phase(PH_PING, 3000)) return 0;
  double v[10]; int n;
  if (w.gateway[0] && (n = icmp_rtt(w.gateway, v, 8)) > 0) { double r = median(v, n, NULL); SDL_LockMutex(mx); M.router = r; SDL_UnlockMutex(mx); }
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
  stopping = 0; __atomic_store_n(&total, 0, __ATOMIC_RELAXED);
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
