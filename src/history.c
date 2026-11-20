// Earlier runs, newest first, in <userdata>/history.txt: one line each, at most MAX_RUNS.
#include "speed.h"
#include <stdio.h>
#include <string.h>

static Run runs[MAX_RUNS];
static int nruns;
static char path[600];

void history_open(const char *userdata) {
  snprintf(path, sizeof path, "%s/history.txt", userdata);
  FILE *f = fopen(path, "r"); nruns = 0;
  if (!f) return;
  char line[300];
  while (nruns < MAX_RUNS && fgets(line, sizeof line, f)) {
    Run r = { 0 };
    if (sscanf(line, "%ld %d %lf %lf %lf %lf %lf %lf %d %d %19s", &r.when, &r.target, &r.down, &r.up, &r.ping, &r.jitter,
               &r.router, &r.low, &r.dbm, &r.freq, r.bssid) >= 10) runs[nruns++] = r;
  }
  fclose(f);
}

static void save(void) {
  char tmp[620]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "w"); if (!f) return;
  for (int i = 0; i < nruns; i++) {
    const Run *r = &runs[i];
    fprintf(f, "%ld %d %.2f %.2f %.1f %.1f %.1f %.2f %d %d %s\n", r->when, r->target, r->down, r->up, r->ping, r->jitter,
            r->router, r->low, r->dbm, r->freq, r->bssid[0] ? r->bssid : "-");
  }
  fclose(f);
  rename(tmp, path);
}

void history_add(const Run *r) {
  memmove(runs + 1, runs, sizeof *runs * (nruns < MAX_RUNS ? nruns : MAX_RUNS - 1));
  runs[0] = *r; if (nruns < MAX_RUNS) nruns++;
  save();
}

int history_count(void) { return nruns; }
const Run *history_get(int i) { return i >= 0 && i < nruns ? &runs[i] : NULL; }
