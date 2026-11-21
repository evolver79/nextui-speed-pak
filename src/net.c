// HTTP/1.1 client for the speed tests: one request per connection, plain TCP or TLS (mbedTLS), a 10 s time limit on
// every read and write so a dead link can't hang a test. Small on purpose: the tests need status, headers and bytes.
#include "speed.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct Conn {
  int fd, tls;
  mbedtls_ssl_context ssl;
  char buf[4096]; int have, at;   // read-ahead (headers come in with the first body bytes)
  long long left;                 // body bytes still to come (-1 unknown: until the server closes)
  int chunked_warned;
};

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt cas;
static mbedtls_ssl_config conf;
static int tls_ready;

double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int net_init(const char *ca_file) {
  // TLS 1.3 (on by default in mbedTLS 3.6) does its key exchange through PSA, which must be set up first
  if (psa_crypto_init() != PSA_SUCCESS) { fprintf(stderr, "net: PSA crypto init failed\n"); return 0; }
  mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg); mbedtls_x509_crt_init(&cas); mbedtls_ssl_config_init(&conf);
  if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"speed", 5)) return 0;
  if (mbedtls_x509_crt_parse_file(&cas, ca_file) < 0) { fprintf(stderr, "net: can't read %s\n", ca_file); return 0; }
  if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
    return 0;
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conf, &cas, NULL);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  tls_ready = 1;
  return 1;
}

static int tcp_send(void *ctx, const unsigned char *p, size_t n) {
  int r = send(*(int *)ctx, p, n, MSG_NOSIGNAL);
  return r < 0 ? -1 : r;
}
static int tcp_recv(void *ctx, unsigned char *p, size_t n) {
  int r = recv(*(int *)ctx, p, n, 0);
  return r < 0 ? -1 : r;
}

// url -> https?, host, port, path
static int parse_url(const char *url, int *tls, char *host, int hn, int *port, const char **path) {
  const char *p;
  if (!strncmp(url, "https://", 8)) { *tls = 1; p = url + 8; *port = 443; }
  else if (!strncmp(url, "http://", 7)) { *tls = 0; p = url + 7; *port = 80; }
  else return 0;
  const char *slash = strchr(p, '/'), *end = slash ? slash : p + strlen(p);
  const char *colon = memchr(p, ':', end - p);
  int len = (colon ? colon : end) - p;
  if (len <= 0 || len >= hn) return 0;
  memcpy(host, p, len); host[len] = 0;
  if (colon) *port = atoi(colon + 1);
  *path = slash ? slash : "/";
  return 1;
}

// TCP (and TLS) to url's host; *path is set to url's path. upload: a small send buffer, so bytes counted as sent are
// close to bytes on the air.
static Conn *conn_open(const char *url, int upload, char *host, int hn, const char **path) {
  int tls, port;
  if (!parse_url(url, &tls, host, hn, &port, path)) return NULL;
  if (tls && !tls_ready) return NULL;
  char ps[8]; snprintf(ps, sizeof ps, "%d", port);
  struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
  if (getaddrinfo(host, ps, &hints, &ai)) return NULL;
  int fd = socket(ai->ai_family, ai->ai_socktype, 0);
  if (fd < 0) { freeaddrinfo(ai); return NULL; }
  struct timeval tv = { 10, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (upload) { int sb = 64 * 1024; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb); }
  int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (connect(fd, ai->ai_addr, ai->ai_addrlen)) { freeaddrinfo(ai); close(fd); return NULL; }
  freeaddrinfo(ai);
  Conn *c = calloc(1, sizeof *c);
  c->fd = fd; c->tls = tls; c->left = -1;
  if (tls) {
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &conf) || mbedtls_ssl_set_hostname(&c->ssl, host)) { conn_close(c); return NULL; }
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, tcp_send, tcp_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
      if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
        char e[100]; mbedtls_strerror(r, e, sizeof e); fprintf(stderr, "net: TLS %s: %s\n", host, e);
        conn_close(c); return NULL;
      }
  }
  return c;
}

Conn *conn_request(const char *method, const char *url, const char *headers, long long body_len) {
  char host[256]; const char *path;
  Conn *c = conn_open(url, body_len > 0, host, sizeof host, &path);
  if (!c) return NULL;
  char req[1500];
  int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
                   "Accept-Encoding: identity\r\nConnection: close\r\n%s", method, path, host, headers ? headers : "");
  if (body_len > 0) n += snprintf(req + n, sizeof req - n, "Content-Type: application/octet-stream\r\nContent-Length: %lld\r\n", body_len);
  n += snprintf(req + n, sizeof req - n, "\r\n");
  if (conn_write(c, req, n) != n) { conn_close(c); return NULL; }
  return c;
}

int conn_write(Conn *c, const void *p, int n) {
  int done = 0;
  while (done < n) {
    int r = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)p + done, n - done)
                   : send(c->fd, (const char *)p + done, n - done, MSG_NOSIGNAL);
    if (c->tls && (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ)) continue;
    if (r <= 0) return -1;
    done += r;
  }
  return done;
}

static int raw_read(Conn *c, void *p, int n) {
  for (;;) {
    int r = c->tls ? mbedtls_ssl_read(&c->ssl, p, n) : recv(c->fd, p, n, 0);
    if (c->tls && (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
    if (c->tls && r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return r;
  }
}

int conn_head(Conn *c) {
  // read until the blank line; whatever comes after it stays in buf for conn_read
  c->have = c->at = 0;
  char *end = NULL;
  while (!end) {
    if (c->have >= (int)sizeof c->buf - 1) return -1;
    int r = raw_read(c, c->buf + c->have, sizeof c->buf - 1 - c->have);
    if (r <= 0) return -1;
    c->have += r; c->buf[c->have] = 0;
    end = strstr(c->buf, "\r\n\r\n");
  }
  int status = 0;
  if (sscanf(c->buf, "HTTP/%*s %d", &status) != 1) return -1;
  for (char *l = strstr(c->buf, "\r\n"); l && l < end; l = strstr(l + 2, "\r\n")) {
    if (!strncasecmp(l + 2, "Content-Length:", 15)) c->left = atoll(l + 17);
    if (!strncasecmp(l + 2, "Transfer-Encoding:", 18) && strstr(l, "chunked") && strstr(l, "chunked") < strstr(l + 2, "\r\n"))
      c->chunked_warned = 1;   // counted as raw bytes: the chunk headers are a few bytes in megabytes
  }
  c->at = end + 4 - c->buf;
  return status;
}

int conn_read(Conn *c, void *p, int n) {
  if (c->left == 0) return 0;
  if (c->left > 0 && n > c->left) n = (int)c->left;
  int r;
  if (c->at < c->have) {
    r = c->have - c->at; if (r > n) r = n;
    memcpy(p, c->buf + c->at, r); c->at += r;
  } else {
    r = raw_read(c, p, n);
    if (r < 0) return -1;
  }
  if (c->left > 0) c->left -= r;
  return r;
}

void conn_close(Conn *c) {
  if (!c) return;
  if (c->tls) { mbedtls_ssl_close_notify(&c->ssl); mbedtls_ssl_free(&c->ssl); }
  close(c->fd);
  free(c);
}

int http_get(const char *url, const char *headers, char *buf, int n) {
  Conn *c = conn_request("GET", url, headers, 0);
  if (!c) return -1;
  int st = conn_head(c), len = 0, r;
  if (st != 200) { conn_close(c); return -1; }
  while (len < n - 1 && (r = conn_read(c, buf + len, n - 1 - len)) > 0) len += r;
  buf[len] = 0;
  int chunked = c->chunked_warned;
  conn_close(c);
  if (chunked) {   // "<hex size>\r\n<data>\r\n" ... "0\r\n\r\n": joined up in place
    int in = 0, out = 0;
    while (in < len) {
      char *e; long size = strtol(buf + in, &e, 16);
      char *nl = strstr(e, "\r\n");
      if (!nl || size <= 0) break;
      in = nl + 2 - buf;
      if (in + size > len) size = len - in;
      memmove(buf + out, buf + in, size); out += size; in += size + 2;
    }
    buf[out] = 0; len = out;
  }
  return len;
}

void conn_abort(Conn *c) { if (c) shutdown(c->fd, SHUT_RDWR); }   // from another thread: its read/write returns now

// Round trip to the server of url: the time a TCP connect takes (SYN, SYN-ACK), n times; the address is looked up once
// so the name lookup isn't timed. Fills ms[]; returns how many worked.
int net_rtt(const char *url, double *ms, int n) {
  int tls, port, ok = 0; char host[256], ps[8]; const char *path;
  if (!parse_url(url, &tls, host, sizeof host, &port, &path)) return 0;
  snprintf(ps, sizeof ps, "%d", port);
  struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
  if (getaddrinfo(host, ps, &hints, &ai)) return 0;
  for (int i = 0; i < n; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) break;
    struct timeval tv = { 3, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    double t0 = now_ms();
    if (!connect(fd, ai->ai_addr, ai->ai_addrlen)) ms[ok++] = now_ms() - t0;
    close(fd);
    usleep(100000);
  }
  freeaddrinfo(ai);
  return ok;
}

