// The Sugar Sense display rules, the same ones every Sugar Sense surface
// follows. Pure C++: no Arduino, no clock, no network, so tools/host_test.py
// compiles and runs them on a computer.
//
//  - glucose travels in mg/dL, mmol/L only at display (x 0.0555)
//  - zones 0 in range / 1 warning / 2 alert, the limits themselves are OUT
//  - stale = STRICTLY more than 15 minutes old, long stale = more than 2 hours
//  - trend arrows 1 down, 2 down-right, 3 flat, 4 up-right, 5 up
//  - the change pairs the two tail points the server sends (the partner and
//    the newest reading), hidden across a gap of more than 20 minutes
#ifndef SS_LOGIC_H
#define SS_LOGIC_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ss {

static const int64_t STALE_MS = 900000;            // 15 min
static const int64_t LONG_STALE_MS = 7200000;      // 2 h
static const int64_t DELTA_MAX_GAP_MS = 1200000;   // 20 min, inclusive
static const int64_t DELTA_MATCH_MS = 30000;       // the tail must BE the headline reading

enum { ZONE_IN_RANGE = 0, ZONE_WARNING = 1, ZONE_ALERT = 2 };

// /getGlucoseSnapshot `state` (wire contract); NONE = nothing pulled yet.
// What the server says the paired account can show. 1 is never sent to a
// display today (the display is free); it stays a named answer.
enum { STATE_NONE = -1, STATE_OK = 0, STATE_UNAVAILABLE = 1, STATE_FOLLOWER = 2, STATE_NO_SOURCE = 3 };

struct Band { int veryLow, low, high, veryHigh; };
static const Band DEFAULT_BAND = {55, 70, 180, 250};

// A reading can be fractional on the wire (a typed mmol/L value stored in
// mg/dL), so the raw value stays a double until the display rounds it.
struct Point { int64_t dateMs; double mgdl; };

static inline int zoneFor(double mgdl, const Band &b) {
  if (mgdl <= b.veryLow || mgdl >= b.veryHigh) return ZONE_ALERT;
  if (mgdl <= b.low || mgdl >= b.high) return ZONE_WARNING;
  return ZONE_IN_RANGE;
}

// A missing or zero date (the null-payload convention) is stale.
static inline bool isStale(int64_t nowMs, int64_t dateMs) {
  if (dateMs <= 0) return true;
  return (nowMs - dateMs) > STALE_MS;
}

static inline bool isLongStale(int64_t nowMs, int64_t dateMs) {
  if (dateMs <= 0) return true;
  return (nowMs - dateMs) > LONG_STALE_MS;
}

// mg/dL -> mmol/L in tenths (100 -> 56 = "5.6"), half away from zero. Doubles
// on purpose: with 32-bit floats 100 mg/dL would read 5.5.
static inline int toMmolTenths(double mgdl) {
  return (int)((mgdl * 0.0555) * 10.0 + 0.5);
}

static inline void formatValue(char *out, size_t n, double mgdl, bool mmol) {
  if (!mmol) {
    snprintf(out, n, "%d", (int)(mgdl + 0.5));
    return;
  }
  int t = toMmolTenths(mgdl);
  snprintf(out, n, "%d.%d", t / 10, t % 10);
}

// Anything unknown renders FLAT, never blank.
static inline int clampArrow(int a) { return (a < 1 || a > 5) ? 3 : a; }

// Whole minutes since the reading, floored at 0; -1 without a date.
static inline int elapsedMinutes(int64_t nowMs, int64_t dateMs) {
  if (dateMs <= 0) return -1;
  int64_t m = (nowMs - dateMs) / 60000;
  return m < 0 ? 0 : (int)m;
}

// The signed change in mg/dL between the two tail points (oldest first).
// False whenever anything is off: a stale headline, fewer than two points, a
// partner further back than the cap, or a tail that is not the headline.
static inline bool deltaFromPoints(const Point *pts, int n, int64_t headlineMs, int64_t nowMs, double *out) {
  if (isStale(nowMs, headlineMs)) return false;
  if (!pts || n < 2) return false;
  const Point &last = pts[n - 1];
  const Point &prev = pts[n - 2];
  if (last.mgdl <= 0 || prev.mgdl <= 0) return false;
  int64_t gap = last.dateMs - prev.dateMs;
  if (gap <= 0 || gap > DELTA_MAX_GAP_MS) return false;
  int64_t off = last.dateMs - headlineMs;
  if (off < 0) off = -off;
  if (off > DELTA_MATCH_MS) return false;
  *out = last.mgdl - prev.mgdl;
  return true;
}

// "+4" / "-5" / "0" in mg/dL, "+0.2" / "-0.3" / "0.0" in mmol/L (the mg/dL
// difference converted, one decimal, half away from zero). `minus` is the
// glyph to print: the display's fonts carry the real minus sign.
static inline void formatDelta(char *out, size_t n, double deltaMgdl, bool mmol, const char *minus) {
  if (!mmol) {
    int whole = deltaMgdl >= 0 ? (int)(deltaMgdl + 0.5) : -(int)((-deltaMgdl) + 0.5);
    if (whole == 0) snprintf(out, n, "0");
    else if (whole > 0) snprintf(out, n, "+%d", whole);
    else snprintf(out, n, "%s%d", minus, -whole);
    return;
  }
  double scaled = (deltaMgdl * 0.0555) * 10.0;
  int tenths = scaled >= 0 ? (int)(scaled + 0.5) : -(int)((-scaled) + 0.5);
  if (tenths == 0) {
    snprintf(out, n, "0.0");
    return;
  }
  int m = tenths < 0 ? -tenths : tenths;
  snprintf(out, n, "%s%d.%d", tenths > 0 ? "+" : minus, m / 10, m % 10);
}

// Which of two firmware versions is newer: > 0 when `a` is, < 0 when `b` is,
// 0 when they are the same. Dotted numbers compared part by part ("0.10.0" is
// newer than "0.9.3"); a missing part counts as 0; anything that is not a
// digit or a dot ends the version.
static inline int compareVersions(const char *a, const char *b) {
  for (int part = 0; part < 4; part++) {
    long x = 0, y = 0;
    while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
    while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
    if (x != y) return x > y ? 1 : -1;
    const bool moreA = *a == '.', moreB = *b == '.';
    if (!moreA && !moreB) return 0;
    if (moreA) a++;
    if (moreB) b++;
  }
  return 0;
}

// Local wall clock from UTC + an offset (the display keeps no zone database:
// the server sends the offset of the phone that paired it, or it is set by
// hand in Settings).
static inline void localHourMinute(int64_t nowMs, int tzMinutes, int *hour, int *minute) {
  int64_t s = nowMs / 1000 + (int64_t)tzMinutes * 60;
  int64_t day = ((s % 86400) + 86400) % 86400;
  *hour = (int)(day / 3600);
  *minute = (int)((day % 3600) / 60);
}

// The night window [start, end) in local hours; start == end means never.
static inline bool isNightHour(int hour, int start, int end) {
  if (start == end) return false;
  return start < end ? (hour >= start && hour < end) : (hour >= start || hour < end);
}

// Which look is up for a Scene setting. Scenes as Settings stores them: 0 Auto
// (the default), 1 Dark, 2 Night, 3 Color. Looks: 0 Dark, 1 Night, 2 Color.
// Auto is the Color scene by day and the Night scene in the night hours; the
// three named scenes are that look at every hour. A value this build does not
// know reads as Auto.
static inline int lookForScene(int scene, bool nightHours) {
  if (scene == 1) return 0;
  if (scene == 2) return 1;
  if (scene == 3) return 2;
  return nightHours ? 1 : 2;
}

// ---- several people on one screen -------------------------------------------
// A follower's display may show up to four people at once (the account's own
// glucose and the people it follows in the Care Circle). The screen splits
// into tiles under a header of `top` pixels: two people share the width on
// the wide screen and stack upright; three and four make two rows, and an
// odd last tile spans its row.
#define SS_MAX_TILES 4

struct Tile { int x, y, w, h; };

static inline int tileLayout(int n, int W, int H, int top, Tile *out) {
  if (n < 1) return 0;
  if (n > SS_MAX_TILES) n = SS_MAX_TILES;
  const bool wide = W > H;
  const int cols = n == 1 ? 1 : ((wide || n == 4) ? 2 : 1);
  const int rows = (n + cols - 1) / cols;
  const int h = (H - top) / rows;
  for (int i = 0; i < n; i++) {
    const int r = i / cols, c = i % cols;
    const bool spans = (i == n - 1) && (n % cols != 0);
    out[i] = {spans ? 0 : c * (W / cols), top + r * h, spans ? W : W / cols, h};
  }
  return n;
}

// The people a display shows, kept as one comma-separated string of ids
// ("me" for the account's own glucose, else the followed person's id). An
// empty string means "nothing chosen yet": the caller applies its default.
static inline bool selectionHas(const char *sel, const char *id) {
  if (!sel || !id || !id[0]) return false;
  const size_t n = strlen(id);
  const char *p = sel;
  while (*p) {
    const char *e = strchr(p, ',');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && strncmp(p, id, n) == 0) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}

static inline int selectionCount(const char *sel) {
  if (!sel || !sel[0]) return 0;
  int n = 1;
  for (const char *p = sel; *p; p++) if (*p == ',') n++;
  return n;
}

// Adds an id that is absent, removes one that is present. Returns false when
// nothing changed: the last chosen person cannot be removed, a fifth cannot
// be added, and the string cannot grow past its room.
static inline bool selectionToggle(char *sel, size_t room, const char *id) {
  if (!sel || !id || !id[0] || strchr(id, ',')) return false;
  if (selectionHas(sel, id)) {
    if (selectionCount(sel) <= 1) return false;
    char out[256];
    size_t o = 0;
    const char *p = sel;
    while (*p) {
      const char *e = strchr(p, ',');
      const size_t len = e ? (size_t)(e - p) : strlen(p);
      if (!(len == strlen(id) && strncmp(p, id, len) == 0)) {
        if (o + len + 2 >= sizeof(out)) return false;
        if (o) out[o++] = ',';
        memcpy(out + o, p, len);
        o += len;
      }
      if (!e) break;
      p = e + 1;
    }
    out[o] = '\0';
    snprintf(sel, room, "%s", out);
    return true;
  }
  if (selectionCount(sel) >= SS_MAX_TILES) return false;
  const size_t need = strlen(sel) + (sel[0] ? 1 : 0) + strlen(id) + 1;
  if (need > room) return false;
  if (sel[0]) strcat(sel, ",");
  strcat(sel, id);
  return true;
}

// The moment in an HTTP Date header ("Thu, 05 Nov 2026 07:22:46 GMT") as
// seconds since 1970, UTC. The display sets its clock from the Date of our
// own server's answers, so it keeps time on a network that blocks NTP.
static inline bool parseHttpDate(const char *text, int64_t *epochSeconds) {
  static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const int LENGTHS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  char name[4] = {0, 0, 0, 0};
  if (!text || sscanf(text, "%*[^,], %d %3s %d %d:%d:%d", &day, name, &year, &hour, &minute, &second) != 6) return false;
  int month = -1;
  for (int i = 0; i < 12; i++)
    if (strcmp(name, MONTHS[i]) == 0) month = i;
  if (month < 0 || year < 2024 || year > 2099 || day < 1 || day > 31) return false;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return false;
  auto leap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
  int64_t days = day - 1;
  for (int y = 1970; y < year; y++) days += leap(y) ? 366 : 365;
  for (int m = 0; m < month; m++) days += LENGTHS[m] + (m == 1 && leap(year) ? 1 : 0);
  *epochSeconds = days * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}

}  // namespace ss

#endif
