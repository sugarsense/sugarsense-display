// Every screen of the Sugar Sense display, as pure drawing functions over gx.
// Nothing here reads a clock, a sensor or the network: the caller hands in a
// View (the last snapshot, the status flags, "now"), so tools/host_test.py can
// render each screen on a computer exactly as the board draws it.
//
// The look is the Sugar Sense app's own, in three scenes:
//   - Dark (the default): the app's dark theme made for a desk. Flat and
//     quiet: the number in its zone color, the arrow, the change and the age
//     in plain type, and the app's graph (a smooth line colored by zone over
//     a soft fill, the target range as a band) in a card.
//   - Night: a black screen, the clock at half the number, for a nightstand.
//   - Color: the app's full screen view, the whole canvas in the zone color
//     with white ink.
// Setup and settings use the app's dark theme: near-black page, slate cards
// with 16 px corners, pill buttons in the brand green.
// The layout functions (glanceLayout, menuLayout, kbBuild ...) are the ONE
// source for both the drawing and the touch hit zones.
#ifndef SS_UI_H
#define SS_UI_H

#include <stdio.h>
#include <string.h>
#include "gx.h"
#include "ss_logic.h"

// Private glyph slots of the text fonts (tools/gen_assets.py SPECIALS).
#define CH_MINUS "\x80"
#define CH_DOT "\x81"
#define CH_ELL "\x82"

namespace ui {

using gx::H;
using gx::W;

// ---- colors -----------------------------------------------------------------
// Chrome: the app's dark theme. The brand green is chrome only.
static const uint16_t C_BG = HEX(0x0A0E11);
static const uint16_t C_CARD = HEX(0x161D24);
static const uint16_t C_RAISED = HEX(0x232D38);
static const uint16_t C_LINE = HEX(0x2C3742);
static const uint16_t C_TEXT = HEX(0xFFFFFF);
static const uint16_t C_DIM = HEX(0x8B95A1);
static const uint16_t C_FAINT = HEX(0x5A6572);
static const uint16_t C_BRAND = HEX(0x34C06E);
static const uint16_t C_ON_BRAND = HEX(0x0E1316);
static const uint16_t C_LOGO_SUGAR = HEX(0xEAF3EC);
// Clinical: the three zone colors, used for a reading and nothing else, and
// the slate a reading wears once it is no longer current.
static const uint16_t C_OK = HEX(0x65B741);
static const uint16_t C_WARN = HEX(0xFD8D38);
static const uint16_t C_ALERT = HEX(0xFF6868);
static const uint16_t C_SLATE = HEX(0x636B78);
static const uint16_t C_NIGHT_STALE = HEX(0x5B626C);
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_BLACK = 0x0000;

// Where a frame's time goes, in microseconds per stage of the Dark scene. The
// board sets `stageClock` (micros); on a computer it stays null and costs nothing.
static uint32_t (*stageClock)() = nullptr;
static uint32_t stageUs[8];
static uint32_t stageT0 = 0;
static inline void stageStart() {
  if (stageClock) stageT0 = stageClock();
}
static inline void stageEnd(int i) {
  if (!stageClock) return;
  uint32_t now = stageClock();
  stageUs[i] = now - stageT0;
  stageT0 = now;
}

static inline uint16_t zoneColor(int zone) {
  return zone == ss::ZONE_ALERT ? C_ALERT : (zone == ss::ZONE_WARNING ? C_WARN : C_OK);
}

struct Rect {
  int x, y, w, h;
  bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// ---- the model ----------------------------------------------------------------
#define SS_MAX_POINTS 64
#define SS_MAX_FOLLOWED 8

// One followed person inside a snapshot (Care Circle): the same fields as the
// account's own reading, under the name the follower gave them. `state` is
// the server's word for this person: 0 a reading (or a pause), 2 not shared
// with this account right now.
struct Person {
  char id[48] = "";
  char label[32] = "";
  int state = ss::STATE_FOLLOWER;
  bool hasValue = false;
  double mgdl = 0;
  int64_t dateMs = 0;
  int arrow = 3;
  bool isMmol = false;
  ss::Band band = ss::DEFAULT_BAND;
  ss::Point pts[SS_MAX_POINTS];
  int npts = 0;
  int hours = 0;
  bool paused = false;   // the person paused sharing: no number, "Paused"
};

// Someone this account follows, for the People page.
struct Followed {
  char id[48] = "";
  char label[32] = "";
};

struct Snapshot {
  int state = ss::STATE_NONE;
  bool hasValue = false;
  double mgdl = 0;
  int64_t dateMs = 0;
  int arrow = 3;
  bool isMmol = false;
  ss::Band band = ss::DEFAULT_BAND;
  ss::Point pts[SS_MAX_POINTS];   // oldest first; the tail is partner + newest
  int npts = 0;
  int hours = 0;                  // the window these points cover (0 = unknown)
  char appUrl[72] = "";           // where to get the app, sent with the "no glucose source" answer
  char latest[12] = "";           // the firmware version the install page serves ("" = not told)
  char installUrl[64] = "";       // and where that page is
  bool hasOffset = false;         // the server knows this display's time zone
  int offsetMin = 0;              // minutes east of UTC right now, when it does
  int pullS = 60;                 // how often the server wants to be asked
  Person people[SS_MAX_TILES];    // the followed people the display asked for, in its own order
  int npeople = 0;
  Followed followed[SS_MAX_FOLLOWED];   // everyone this account follows (the People page lists them)
  int nfollowed = 0;
};

struct Status {
  bool wifi = true;         // associated with the access point
  bool apiOk = true;        // the last pull reached the server
  bool timeSynced = true;   // the wall clock is known
};

struct View {
  const Snapshot *snap;
  Status st;
  int64_t nowMs;
  int tzMin;
  int graphHours;
  bool showGraph = true;   // false = numbers only: no graph, a larger number in the middle
  const char *personLabel = nullptr;   // the single view of a followed person: their name in place of the wordmark
  // The "one more step" screen (an account with no glucose source yet): the
  // QR code of the page where a phone gets the app, and that page as text.
  const uint8_t *qr = nullptr;
  int qrN = 0;
  const char *qrLabel = nullptr;
};

// ---- small pieces -----------------------------------------------------------
// Copies `src` into `out`, cut with an ellipsis when it is wider than maxW.
static void fitText(const AAFont &f, const char *src, int maxW, char *out, size_t n) {
  snprintf(out, n, "%s", src);
  if (gx::textW(f, out) <= maxW) return;
  size_t len = strlen(out);
  while (len > 1) {
    len--;
    out[len] = '\0';
    char probe[80];
    snprintf(probe, sizeof(probe), "%s" CH_ELL, out);
    if (gx::textW(f, probe) <= maxW) {
      snprintf(out, n, "%s", probe);
      return;
    }
  }
}

// Text from outside (a network name) can carry UTF-8, and the fonts are one
// 8-bit range whose slots above ASCII are the private glyphs: fold a curly
// apostrophe or quote to its ASCII twin and any other sequence to one '?'.
static void asciiLabel(const char *src, char *out, size_t n) {
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)src;
  while (*p && o + 1 < n) {
    if (*p < 0x80) {
      out[o++] = (*p >= 0x20 && *p < 0x7F) ? (char)*p : '?';
      p++;
      continue;
    }
    char rep = '?';
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) rep = '\'';
    else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x9C || p[2] == 0x9D)) rep = '"';
    else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) rep = '-';
    out[o++] = rep;
    p++;
    while ((*p & 0xC0) == 0x80) p++;   // the rest of this sequence
  }
  out[o] = '\0';
}

// The app's pill button: brand green with dark ink, or a quiet slate one.
static void drawButton(const Rect &r, const char *label, bool primary) {
  gx::rrect(r.x, r.y, r.w, r.h, r.h / 2, primary ? C_BRAND : C_RAISED);
  gx::textC(F_MED, r.x + r.w / 2, r.y + r.h / 2 + 8, label, primary ? C_ON_BRAND : C_TEXT);
}

// The drop mark and the two-tone wordmark; returns the x after it.
static int drawLockup(int x, int base, uint16_t mark, uint16_t sugar, uint16_t sense, int alpha = 255) {
  x = gx::text(F_MARK, x, base + 5, "M", mark, alpha) + 7;
  x = gx::text(F_MED, x, base, "Sugar ", sugar, alpha);
  return gx::text(F_MED, x, base, "Sense", sense, alpha);
}

static Rect settingsButton() { return {W - 60, 0, 60, 48}; }

// The single view of a followed person wears their name where the wordmark
// sits (the dots and the clock keep their places). `maxW` 0 = the usual room;
// the Night scene passes the room left of its big clock, in the smaller font.
static void drawPersonLabel(const View &v, uint16_t ink, int alpha, int maxW = 0, const AAFont *font = nullptr) {
  const AAFont &f = font ? *font : F_MED;
  char cut[32];
  fitText(f, v.personLabel, maxW > 0 ? maxW : (W > H ? 200 : 150), cut, sizeof(cut));
  gx::text(f, 14, 31, cut, ink, alpha);
}

// A newer firmware exists: a small badge at the upper right of the three
// dots, in the brand green (or the scene's own ink on a colored canvas). The
// app sets this before it draws the home screen, and leaves it off at night.
static bool updateHint = false;

static void drawDots(uint16_t ink, int alpha) {
  for (int i = 0; i < 3; i++) gx::disc(W - 36 + i * 10, 24, 2.4f, ink, alpha);
  if (updateHint) gx::disc(W - 9, 13, 3.6f, ink == C_DIM ? C_BRAND : ink, 255);
}

static void clockText(const View &v, char *out, size_t n) {
  int hh, mm;
  ss::localHourMinute(v.nowMs, v.tzMin, &hh, &mm);
  snprintf(out, n, "%02d:%02d", hh, mm);
}

// ---- the headline reading ---------------------------------------------------
struct Headline {
  char value[8];    // "104" or "5.8"; empty while there is nothing current
  int arrow;        // 1..5, 0 = none
  int zone;
  char unit[8];
  char delta[12];   // "" when there is no change to show
  char note[32];    // "2 min ago", "No data for 22 min" ...
  bool fresh;       // a current reading: zone color, arrow, change
};

static Headline headline(const View &v) {
  Headline h;
  memset(&h, 0, sizeof(h));
  const Snapshot &s = *v.snap;
  snprintf(h.unit, sizeof(h.unit), "%s", s.isMmol ? "mmol/L" : "mg/dL");
  if (!s.hasValue || !v.st.timeSynced) {
    snprintf(h.note, sizeof(h.note), "Waiting for the first reading");
    return h;
  }
  int mins = ss::elapsedMinutes(v.nowMs, s.dateMs);
  // Past 15 minutes nothing may read as current: no number, no zone color.
  if (ss::isStale(v.nowMs, s.dateMs)) {
    if (ss::isLongStale(v.nowMs, s.dateMs) || mins < 0) snprintf(h.note, sizeof(h.note), "No readings for 2+ hours");
    else snprintf(h.note, sizeof(h.note), "No data for %d min", mins);
    return h;
  }
  h.fresh = true;
  ss::formatValue(h.value, sizeof(h.value), s.mgdl, s.isMmol);
  h.arrow = ss::clampArrow(s.arrow);
  h.zone = ss::zoneFor(s.mgdl, s.band);
  double d = 0;
  if (ss::deltaFromPoints(s.pts, s.npts, s.dateMs, v.nowMs, &d)) ss::formatDelta(h.delta, sizeof(h.delta), d, s.isMmol, CH_MINUS);
  if (mins <= 0) snprintf(h.note, sizeof(h.note), "Just now");
  else snprintf(h.note, sizeof(h.note), "%d min ago", mins);
  return h;
}

// The number's size holds steady for a unit: the largest of the three fonts
// whose WIDEST value ("888" or "88.8") fits beside its arrow.
struct NumStyle {
  const AAFont *num, *arrow;
  int cap;       // the digits' height above the baseline
  int arrowPx;
  int track;     // pixels between digits (large bold numerals sit a little tighter)
};

// The five scene sizes, then three smaller ones for the tiles of the split
// screen (several people at once).
enum { NUM_XL = 0, NUM_L, NUM_X, NUM_M, NUM_S, NUM_T1, NUM_T2, NUM_T3, NUM_COUNT };
static const NumStyle NUM_STYLES[NUM_COUNT] = {{&F_NUM_XL, &F_ARROW_L, 134, 78, -4},
                                               {&F_NUM_L, &F_ARROW_L, 109, 78, -3},
                                               {&F_NUM_X, &F_ARROW_M, 96, 60, -3},
                                               {&F_NUM_M, &F_ARROW_M, 84, 60, -2},
                                               {&F_NUM_S, &F_ARROW_S, 73, 52, -2},
                                               {&F_NUM_T1, &F_ARROW_XS, 64, 34, -2},
                                               {&F_NUM_T2, &F_ARROW_XS, 50, 34, -1},
                                               {&F_NUM_T3, &F_ARROW_XXS, 39, 26, -1}};

// The largest size, from `first` down, whose widest value fits `room` beside
// an arrow (`arrowPx` 0 = the size's own arrow).
static NumStyle fitNum(bool mmol, int first, int room, int arrowPx) {
  const char *widest = mmol ? "88.8" : "888";
  for (int i = first; i < NUM_COUNT; i++) {
    const NumStyle &n = NUM_STYLES[i];
    if (gx::textW(*n.num, widest, n.track) + 10 + (arrowPx ? arrowPx : n.arrowPx) <= room) return n;
  }
  return NUM_STYLES[NUM_COUNT - 1];
}

// The Color and Night scenes: with a graph the wide screen takes the 150 px
// number by day and a smaller one at night (the clock needs the room);
// without a graph everything goes one step up.
static NumStyle numStyle(bool mmol, bool night, bool graph) {
  const bool wide = W > H;
  const int first = !wide ? NUM_M : (graph ? (night ? NUM_M : NUM_L) : (night ? NUM_L : NUM_XL));
  return fitNum(mmol, first, W - 20, 0);
}

// The number with its trend arrow, centred on cx as one group.
static void drawNumber(int cx, int base, const Headline &h, const NumStyle &ns, uint16_t ink) {
  int vw = gx::textW(*ns.num, h.value, ns.track);
  int total = vw + (h.arrow ? 10 + ns.arrowPx : 0);
  int x = cx - total / 2;
  gx::text(*ns.num, x, base, h.value, ink, 255, ns.track);
  if (h.arrow) {
    char a[2] = {(char)('0' + h.arrow), 0};
    gx::text(*ns.arrow, x + vw + 10, base - (ns.cap - ns.arrowPx) / 2, a, ink);
  }
}

// "+7 mg/dL" (the change bolder, then the unit), optionally " · 2 min ago".
static void drawChangeLine(int cx, int base, const Headline &h, bool withNote, uint16_t ink, int noteAlpha) {
  const char *sep = "  " CH_DOT "  ";
  int wd = h.delta[0] ? gx::textW(F_TITLE, h.delta) + 8 : 0;
  int total = wd + gx::textW(F_MED, h.unit);
  if (withNote) total += gx::textW(F_MED, sep) + gx::textW(F_MED, h.note);
  int x = cx - total / 2;
  if (h.delta[0]) x = gx::text(F_TITLE, x, base, h.delta, ink) + 8;
  x = gx::text(F_MED, x, base, h.unit, ink, 216);
  if (withNote) {
    x = gx::text(F_MED, x, base, sep, ink, 120);
    gx::text(F_MED, x, base, h.note, ink, noteAlpha);
  }
}

// A smooth curve through points with rising x that never overshoots them
// (monotone cubic interpolation, Fritsch and Carlson).
struct Curve {
  int n = 0;
  float x[SS_MAX_POINTS], y[SS_MAX_POINTS], m[SS_MAX_POINTS];

  void add(float px, float py) {
    if (n >= SS_MAX_POINTS || (n > 0 && px <= x[n - 1])) return;
    x[n] = px;
    y[n] = py;
    n++;
  }

  void finish() {
    if (n < 2) return;
    float d[SS_MAX_POINTS];
    for (int i = 0; i < n - 1; i++) d[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; i++) m[i] = d[i - 1] * d[i] <= 0.0f ? 0.0f : (d[i - 1] + d[i]) * 0.5f;
    for (int i = 0; i < n - 1; i++) {
      if (d[i] == 0.0f) {
        m[i] = m[i + 1] = 0.0f;
        continue;
      }
      float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
      if (s > 9.0f) {
        float t = 3.0f / sqrtf(s);
        m[i] = t * a * d[i];
        m[i + 1] = t * b * d[i];
      }
    }
  }

  float at(float px) const {
    if (n == 1 || px <= x[0]) return y[0];
    if (px >= x[n - 1]) return y[n - 1];
    int i = 0;
    while (i < n - 2 && px > x[i + 1]) i++;
    float h = x[i + 1] - x[i], t = (px - x[i]) / h, t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * y[i + 1] + (t3 - t2) * h * m[i + 1];
  }
};

// ---- the trend card -----------------------------------------------------------
// The app's full screen trend: the target range as a soft band, a fill fading
// down from the line, the line itself and a dot on the newest reading, all in
// the scene's ink. A taller card also carries the window, the hours and the
// two limits.
static void drawTrend(const Rect &r, const View &v, uint16_t ink, int cardAlpha, bool fresh) {
  const Snapshot &s = *v.snap;
  gx::rrect(r.x, r.y, r.w, r.h, 20, ink, cardAlpha);
  const bool labels = r.h >= 96;
  const int px0 = r.x + 14, px1 = r.x + r.w - (labels ? 46 : 14);
  const int py0 = r.y + (labels ? 32 : 12), py1 = r.y + r.h - (labels ? 30 : 12);
  const int64_t winMs = (int64_t)v.graphHours * 3600000;
  const int64_t t0 = v.nowMs - winMs;
  if (labels) {
    char lab[8];
    snprintf(lab, sizeof(lab), "%dh", v.graphHours);
    gx::text(F_SMALL, r.x + 14, r.y + 23, lab, ink, 190);
  }

  // the scale always shows the whole target range, and every reading
  double vmin = s.band.low, vmax = s.band.high;
  int shown = 0;
  for (int i = 0; i < s.npts; i++) {
    if (s.pts[i].dateMs < t0 || s.pts[i].dateMs > v.nowMs) continue;
    shown++;
    if (s.pts[i].mgdl > vmax) vmax = s.pts[i].mgdl;
    if (s.pts[i].mgdl < vmin) vmin = s.pts[i].mgdl;
  }
  vmin -= 14;
  vmax += 14;
  auto Y = [&](double mg) { return (float)py1 - (float)((mg - vmin) / (vmax - vmin) * (double)(py1 - py0)); };
  auto X = [&](int64_t t) { return (float)px0 + (float)((double)(t - t0) / (double)winMs * (double)(px1 - px0)); };

  int yh = (int)(Y(s.band.high) + 0.5f), yl = (int)(Y(s.band.low) + 0.5f);
  gx::rect(px0, yh, px1 - px0, yl - yh, ink, 30);
  gx::dashH(px0, yh, px1 - px0, 4, 4, ink, 110);
  gx::dashH(px0, yl, px1 - px0, 4, 4, ink, 110);
  if (labels) {
    char b[8];
    ss::formatValue(b, sizeof(b), s.band.high, s.isMmol);
    gx::text(F_SMALL, px1 + 8, yh + 5, b, ink, 190);
    ss::formatValue(b, sizeof(b), s.band.low, s.isMmol);
    gx::text(F_SMALL, px1 + 8, yl + 5, b, ink, 190);
    if (v.st.timeSynced) {
      const int step = v.graphHours <= 6 ? 1 : (v.graphHours == 12 ? 3 : 6);
      const int64_t tz = (int64_t)v.tzMin * 60000;
      for (int64_t t = (((t0 + tz) / 3600000) + 1) * 3600000 - tz; t < v.nowMs; t += 3600000) {
        int hour = (int)((((t + tz) / 3600000) % 24 + 24) % 24);
        if (hour % step) continue;
        int x = (int)(X(t) + 0.5f);
        if (x < px0 + 8 || x > px1 - 8) continue;
        char hb[4];
        snprintf(hb, sizeof(hb), "%02d", hour);
        gx::textC(F_SMALL, x, r.y + r.h - 9, hb, ink, 150);
      }
    }
  }
  if (!shown) {
    gx::textC(F_SMALL, (px0 + px1) / 2, (py0 + py1) / 2 + 5, "No readings in this window", ink, 180);
    return;
  }

  // runs of readings; a pause longer than three steps of this window splits them
  const int64_t gapMs = (int64_t)(v.graphHours <= 3 ? 5 : (v.graphHours == 6 ? 10 : (v.graphHours == 12 ? 15 : 30))) * 60000 * 3;
  int i = 0, last = -1;
  while (i < s.npts) {
    if (s.pts[i].dateMs < t0 || s.pts[i].dateMs > v.nowMs) {
      i++;
      continue;
    }
    Curve c;
    int j = i;
    for (; j < s.npts && s.pts[j].dateMs <= v.nowMs; j++) {
      if (j > i && s.pts[j].dateMs - s.pts[j - 1].dateMs > gapMs) break;
      c.add(X(s.pts[j].dateMs), Y(s.pts[j].mgdl));
      last = j;
    }
    c.finish();
    if (c.n == 1) {
      gx::disc(c.x[0], c.y[0], 1.8f, ink);   // a lone reading still shows
    } else if (c.n > 1) {
      const int xa = (int)ceilf(c.x[0]), xb = (int)floorf(c.x[c.n - 1]);
      for (int x = xa; x <= xb; x++) gx::fadeDown(x, (int)c.at((float)x) + 1, py1 + 8, ink, 64);   // the fill under the line
      float px = c.x[0], py = c.y[0];
      for (int x = xa; x <= xb + 1; x++) {
        float nx = x > xb ? c.x[c.n - 1] : (float)x, ny = c.at(nx);
        gx::line(px, py, nx, ny, 2.8f, ink);
        px = nx;
        py = ny;
      }
    }
    i = j > i ? j : i + 1;
  }
  // the newest reading wears its dot only while it is current
  if (fresh && last >= 0) gx::disc(X(s.pts[last].dateMs), Y(s.pts[last].mgdl), 5.2f, ink);
}

// ---- the glance screen: the app's full screen glucose view ---------------------
struct GlanceLayout {
  int clockBase;   // night only: the big clock above the number
  int numBase;
  int lineBase;    // the change + unit line
  int noteBase;    // the age line; 0 = it rides the change line
  Rect trend;
};

static GlanceLayout glanceLayout(bool night, int cap, bool graph) {
  GlanceLayout L;
  const bool port = H >= W;
  if (!graph) {
    // numbers only: the clock (at night), the number and its lines as one
    // block in the middle of the screen
    const int lines = port ? 42 + 28 : 44, gapClock = port ? 40 : 26;
    if (!night) {
      const int top = 44 + (H - 44 - 12 - (cap + lines)) / 2;
      L.clockBase = 0;
      L.numBase = top + cap;
    } else {
      const int top = (H - (42 + gapClock + cap + lines)) / 2;
      L.clockBase = top + 42;
      L.numBase = L.clockBase + gapClock + cap;
    }
    L.lineBase = L.numBase + (port ? 42 : 44);
    L.noteBase = port ? L.lineBase + 28 : 0;
    L.trend = {0, 0, 0, 0};
    return L;
  }
  if (!night) {
    L.clockBase = 0;
    L.numBase = port ? 96 + cap : 50 + cap;
    L.lineBase = L.numBase + (port ? 42 : 38);
    L.noteBase = port ? L.lineBase + 28 : 0;
    int top = (port ? L.noteBase + 22 : L.lineBase + 18);
    L.trend = {12, top, W - 24, H - top - 12};
  } else {
    L.clockBase = port ? 86 : 56;
    L.numBase = L.clockBase + (port ? 34 : 22) + cap;
    L.lineBase = L.numBase + (port ? 42 : 38);
    L.noteBase = port ? L.lineBase + 28 : 0;
    int top = (port ? L.noteBase + 22 : L.lineBase + 16);
    L.trend = {12, top, W - 24, H - top - 12};
  }
  return L;
}

// What sits in the top left corner when the display has lost its way out.
static void drawLinkWarning(const View &v, uint16_t ink, int alpha) {
  const char *t = !v.st.wifi ? "No Wi-Fi" : "Offline";
  int w = gx::textW(F_SMALL, t);
  gx::rrect(10, 10, w + 26, 28, 14, C_BLACK, 76);
  gx::text(F_SMALL, 23, 30, t, ink, alpha);
}

// The Color scene: the canvas is the zone color, the ink white. A reading
// that is no longer current sits on slate and says so.
static void drawDay(const View &v) {
  Headline h = headline(v);
  NumStyle ns = numStyle(v.snap->isMmol, false, v.showGraph);
  GlanceLayout L = glanceLayout(false, ns.cap, v.showGraph);
  const uint16_t ink = C_WHITE;
  gx::clear(h.fresh ? zoneColor(h.zone) : C_SLATE);

  if (!v.st.wifi || !v.st.apiOk) drawLinkWarning(v, ink, 255);
  else if (v.personLabel) drawPersonLabel(v, ink, 236);
  else drawLockup(14, 31, ink, ink, ink, 236);
  drawDots(ink, 190);
  if (v.st.timeSynced) {
    char t[8];
    clockText(v, t, sizeof(t));
    gx::textR(F_MED, W - 56, 31, t, ink, 190);
  }

  if (h.fresh) {
    drawNumber(W / 2, L.numBase, h, ns, ink);
    drawChangeLine(W / 2, L.lineBase, h, L.noteBase == 0, ink, 190);
    if (L.noteBase) gx::textC(F_MED, W / 2, L.noteBase, h.note, ink, 190);
  } else {
    gx::textC(F_HEAD, W / 2, L.numBase - (ns.cap - 46) / 2, "No Data", ink);
    gx::textC(F_MED, W / 2, L.lineBase, h.note, ink, 205);
  }
  if (v.showGraph) drawTrend(L.trend, v, ink, 40, h.fresh);
}

// The Night scene: black, the clock at about half the number, everything in
// the zone color, a soft glow behind a number that is out of range.
static void drawNight(const View &v) {
  Headline h = headline(v);
  NumStyle ns = numStyle(v.snap->isMmol, true, v.showGraph);
  GlanceLayout L = glanceLayout(true, ns.cap, v.showGraph);
  const uint16_t ink = h.fresh ? zoneColor(h.zone) : C_NIGHT_STALE;
  gx::clear(C_BLACK);

  if (!v.st.wifi || !v.st.apiOk) {
    drawLinkWarning(v, C_DIM, 255);
  } else if (v.personLabel) {
    // On the wide screen the big clock reaches the top row: the name keeps
    // to the room on its left, in the smaller font.
    const int clockTop = L.clockBase - 44;
    if (v.st.timeSynced && clockTop < 40) {
      char t[8];
      clockText(v, t, sizeof(t));
      drawPersonLabel(v, C_DIM, 255, (W - gx::textW(F_CLOCK, t)) / 2 - 14 - 12, &F_SMALL);
    } else {
      drawPersonLabel(v, C_DIM, 255);
    }
  }
  drawDots(C_DIM, 150);
  if (v.st.timeSynced) {
    char t[8];
    clockText(v, t, sizeof(t));
    gx::textC(F_CLOCK, W / 2, L.clockBase, t, ink, 204);
  }

  if (h.fresh) {
    if (h.zone != ss::ZONE_IN_RANGE) gx::glow(W / 2, L.numBase - ns.cap / 2, ns.cap * 1.5f, ns.cap * 1.5f, ink, 36);
    drawNumber(W / 2, L.numBase, h, ns, ink);
    drawChangeLine(W / 2, L.lineBase, h, L.noteBase == 0, ink, 170);
    if (L.noteBase) gx::textC(F_MED, W / 2, L.noteBase, h.note, ink, 170);
  } else {
    gx::textC(F_HEAD, W / 2, L.numBase - (ns.cap - 46) / 2, "No Data", ink);
    gx::textC(F_MED, W / 2, L.lineBase, h.note, ink, 220);
  }
  if (v.showGraph) drawTrend(L.trend, v, ink, 24, h.fresh);
}

// The three scenes of the glance screen.
enum { LOOK_DARK = 0, LOOK_NIGHT = 1, LOOK_COLOR = 2 };

// ---- the Dark scene ---------------------------------------------------------------
// The line's color at a glucose value: the zone color, eased over a few mg/dL
// around each limit so the line turns instead of snapping.
static uint16_t curveColor(double mgdl, const ss::Band &b) {
  auto ease = [](double edge, double v, double dir) {
    double t = (v - edge) * dir / 8.0 + 0.5;
    return (float)(t < 0 ? 0 : (t > 1 ? 1 : t));
  };
  float warn = ease(b.high, mgdl, 1), alert = ease(b.veryHigh, mgdl, 1);
  float lowWarn = ease(b.low, mgdl, -1), lowAlert = ease(b.veryLow, mgdl, -1);
  if (lowWarn > warn) warn = lowWarn;
  if (lowAlert > alert) alert = lowAlert;
  return gx::lerp(gx::lerp(C_OK, C_WARN, warn), C_ALERT, alert);
}

// The app's graph in a card: the target range as a band between two dashed
// limits, a smooth line colored by zone over a fill that fades down, a ringed
// dot on the newest reading while it is current, the hours along the bottom.
// The app's graph in a card: the target range as a band between two dashed
// limits, a smooth line colored by zone over a fill that fades down, a ringed
// dot on the newest reading while it is current, the hours along the bottom.
static void drawChart(const Rect &r, const View &v, bool fresh) {
  const Snapshot &s = *v.snap;
  gx::rrect(r.x, r.y, r.w, r.h, 18, C_CARD);

  const int px0 = r.x + 14, px1 = r.x + r.w - 50;   // the limits' values sit right of the plot
  const int py0 = r.y + 18, py1 = r.y + r.h - 30;
  const int64_t winMs = (int64_t)v.graphHours * 3600000;
  const int64_t t0 = v.nowMs - winMs;

  double vmin = s.band.low, vmax = s.band.high;
  int shown = 0;
  for (int i = 0; i < s.npts; i++) {
    if (s.pts[i].dateMs < t0 || s.pts[i].dateMs > v.nowMs) continue;
    shown++;
    if (s.pts[i].mgdl > vmax) vmax = s.pts[i].mgdl;
    if (s.pts[i].mgdl < vmin) vmin = s.pts[i].mgdl;
  }
  const double pad = (vmax - vmin) * 0.12 + 6;
  vmin -= pad;
  vmax += pad;
  auto Y = [&](double mg) { return (float)py1 - (float)((mg - vmin) / (vmax - vmin) * (double)(py1 - py0)); };
  auto X = [&](int64_t t) { return (float)px0 + (float)((double)(t - t0) / (double)winMs * (double)(px1 - px0)); };
  auto valueAt = [&](float y) { return vmin + (double)(py1 - y) / (double)(py1 - py0) * (vmax - vmin); };

  // the target range
  const int yh = (int)(Y(s.band.high) + 0.5f), yl = (int)(Y(s.band.low) + 0.5f);
  gx::rect(px0, yh, px1 - px0, yl - yh, C_OK, 22);
  gx::dashH(px0, yh, px1 - px0, 5, 4, C_OK, 120);
  gx::dashH(px0, yl, px1 - px0, 5, 4, C_OK, 120);
  char b[8];
  ss::formatValue(b, sizeof(b), s.band.high, s.isMmol);
  gx::text(F_SMALL, px1 + 16, yh + 5, b, C_DIM);
  ss::formatValue(b, sizeof(b), s.band.low, s.isMmol);
  gx::text(F_SMALL, px1 + 16, yl + 5, b, C_DIM);

  // the hours, in local time, and the window in the corner
  if (v.st.timeSynced) {
    const int step = v.graphHours <= 3 ? 1 : (v.graphHours == 6 ? 2 : (v.graphHours == 12 ? 3 : 6));
    const int64_t tz = (int64_t)v.tzMin * 60000;
    for (int64_t t = (((t0 + tz) / 3600000) + 1) * 3600000 - tz; t < v.nowMs; t += 3600000) {
      int hour = (int)((((t + tz) / 3600000) % 24 + 24) % 24);
      if (hour % step) continue;
      int x = (int)(X(t) + 0.5f);
      if (x < px0 + 18 || x > px1 - 18) continue;
      gx::rect(x, py0, 1, py1 - py0, C_WHITE, 12);
      char hb[8];
      snprintf(hb, sizeof(hb), "%02d:00", hour);
      gx::textC(F_SMALL, x, r.y + r.h - 10, hb, C_FAINT);
    }
  }
  char lab[8];
  snprintf(lab, sizeof(lab), "%dH", v.graphHours);
  int lw = gx::textW(F_TINY, lab, 1);
  gx::rrect(r.x + r.w - 14 - lw - 16, r.y + r.h - 25, lw + 16, 18, 9, C_RAISED);
  gx::text(F_TINY, r.x + r.w - 14 - lw - 8, r.y + r.h - 12, lab, C_DIM, 255, 1);

  if (!shown) {
    gx::textC(F_SMALL, (px0 + px1) / 2, (py0 + py1) / 2 + 5, "No readings in this window", C_DIM);
    return;
  }

  // the zone color of every row of the card, once
  static uint16_t rowColor[gx::NATIVE_H];
  for (int y = 0; y < r.h && y < gx::NATIVE_H; y++) rowColor[y] = curveColor(valueAt((float)(r.y + y)), s.band);

  // runs of readings; a pause longer than three steps of this window splits them
  const int64_t gapMs = (int64_t)(v.graphHours <= 3 ? 5 : (v.graphHours == 6 ? 10 : (v.graphHours == 12 ? 15 : 30))) * 60000 * 3;
  int i = 0, last = -1;
  while (i < s.npts) {
    if (s.pts[i].dateMs < t0 || s.pts[i].dateMs > v.nowMs) {
      i++;
      continue;
    }
    Curve c;
    int j = i;
    for (; j < s.npts && s.pts[j].dateMs <= v.nowMs; j++) {
      if (j > i && s.pts[j].dateMs - s.pts[j - 1].dateMs > gapMs) break;
      c.add(X(s.pts[j].dateMs), Y(s.pts[j].mgdl));
      last = j;
    }
    c.finish();
    if (c.n == 1) {
      gx::disc(c.x[0], c.y[0], 2.2f, curveColor(valueAt(c.y[0]), s.band));
    } else if (c.n > 1) {
      const int xa = (int)ceilf(c.x[0]), xb = (int)floorf(c.x[c.n - 1]), floorY = py1 + 10;
      for (int x = xa; x <= xb; x++) {   // the fill first, so the line sits on top
        const float ly = c.at((float)x);
        const int top = (int)ly + 1;
        // Every pixel wears the color of its own height, so the zones lie in
        // bands under the line. Below the low limit the fill keeps the color
        // it had there, unless the line itself went low: an in-range line
        // must not stand on a red floor.
        const double lineValue = valueAt(ly), hold = lineValue < s.band.low + 8 ? lineValue : s.band.low + 8;
        const int holdY = (int)Y(hold);
        const uint16_t holdColor = curveColor(hold, s.band);
        for (int y = top; y < floorY; y++)
          gx::blendSoft(x, y, y > holdY ? holdColor : rowColor[y - r.y], 92 * (floorY - y) / (floorY - top));
      }
      float px = c.x[0], py = c.y[0];
      for (int x = xa; x <= xb + 1; x++) {
        float nx = x > xb ? c.x[c.n - 1] : (float)x, ny = c.at(nx);
        gx::line(px, py, nx, ny, 3.2f, curveColor(valueAt((py + ny) * 0.5f), s.band));
        px = nx;
        py = ny;
      }
    }
    i = j > i ? j : i + 1;
  }
  if (fresh && last >= 0) {
    const float x = X(s.pts[last].dateMs), y = Y(s.pts[last].mgdl);
    const uint16_t c = zoneColor(ss::zoneFor(s.pts[last].mgdl, s.band));
    gx::disc(x, y, 12.0f, c, 56);
    gx::disc(x, y, 6.6f, C_WHITE);
    gx::disc(x, y, 4.0f, c);
  }
}

struct DarkLayout {
  NumStyle ns;
  const AAFont *arrow;
  int arrowPx;
  bool side;     // the change and the age stand in a column right of the number
  int numX;      // with the column: the number's left edge; else the centre line
  int numBase;
  int lineBase;  // without the column: the change line under the number
  Rect chart;
};

static DarkLayout darkLayout(bool mmol, bool graph) {
  DarkLayout L;
  const bool wide = W > H;
  if (!graph) {
    // numbers only: the largest number that fits, in the middle, its line under it
    L.ns = fitNum(mmol, wide ? NUM_XL : NUM_M, W - 24, 0);
    L.arrow = L.ns.arrow;
    L.arrowPx = L.ns.arrowPx;
    L.side = false;
    L.numX = W / 2;
    L.numBase = 44 + (H - 44 - 12 - (L.ns.cap + 48)) / 2 + L.ns.cap;
    L.lineBase = L.numBase + 48;
    L.chart = {0, 0, 0, 0};
    return L;
  }
  // landscape: the number and its arrow leave the right side to the change and the age
  L.ns = fitNum(mmol, NUM_X, wide ? 316 : W - 24, 52);
  L.arrow = &F_ARROW_S;
  L.arrowPx = 52;
  L.side = wide;
  if (wide) {
    L.numX = 22;
    L.numBase = 52 + 96;   // every size sits on the baseline of the largest
    L.lineBase = 0;
    L.chart = {12, 170, W - 24, H - 182};
  } else {
    L.numX = W / 2;
    L.numBase = 58 + 84;
    L.lineBase = L.numBase + 42;
    L.chart = {12, L.lineBase + 24, W - 24, H - (L.lineBase + 24) - 12};
  }
  return L;
}

// The Dark scene: nothing decorative. The number in its zone color, the
// arrow beside it, the change and the age in plain type, the graph.
static void drawDark(const View &v) {
  const Snapshot &s = *v.snap;
  Headline h = headline(v);
  DarkLayout L = darkLayout(s.isMmol, v.showGraph);
  stageStart();
  gx::clear(C_BG);

  if (!v.st.wifi || !v.st.apiOk) drawLinkWarning(v, C_TEXT, 255);
  else if (v.personLabel) drawPersonLabel(v, C_TEXT, 230);
  else drawLockup(14, 31, C_BRAND, C_LOGO_SUGAR, C_BRAND);
  drawDots(C_DIM, 255);
  if (v.st.timeSynced) {
    char t[8];
    clockText(v, t, sizeof(t));
    gx::textR(F_MED, W - 56, 31, t, C_TEXT, 210);
  }
  stageEnd(0);

  if (h.fresh) {
    const uint16_t ink = zoneColor(h.zone);
    const int numW = gx::textW(*L.ns.num, h.value, L.ns.track);
    const int x = L.side ? L.numX : L.numX - (numW + 10 + L.arrowPx) / 2;
    gx::text(*L.ns.num, x, L.numBase, h.value, ink, 255, L.ns.track);
    char a[2] = {(char)('0' + h.arrow), 0};
    gx::text(*L.arrow, x + numW + 10, L.numBase - (L.ns.cap - L.arrowPx) / 2, a, ink);
    if (L.side) {
      // the change, its unit and the age, stacked against the right edge
      const int xr = W - 22, mid = L.numBase - 96 / 2;
      if (h.delta[0]) {
        gx::textR(F_TITLE, xr, mid - 14, h.delta, C_TEXT);
        gx::textR(F_MED, xr, mid + 12, h.unit, C_DIM);
        gx::textR(F_MED, xr, mid + 38, h.note, C_DIM);
      } else {
        gx::textR(F_MED, xr, mid - 2, h.unit, C_DIM);
        gx::textR(F_MED, xr, mid + 24, h.note, C_DIM);
      }
    } else {
      drawChangeLine(W / 2, L.lineBase, h, true, C_TEXT, 150);
    }
  } else if (L.side) {
    gx::text(F_HEAD, L.numX, L.numBase - 34, "No Data", C_SLATE);
    gx::text(F_MED, L.numX + 3, L.numBase - 2, h.note, C_DIM);
  } else {
    gx::textC(F_HEAD, W / 2, L.numBase - (L.ns.cap - 46) / 2, "No Data", C_SLATE);
    gx::textC(F_MED, W / 2, L.lineBase, h.note, C_DIM);
  }
  stageEnd(1);
  if (v.showGraph) drawChart(L.chart, v, h.fresh);
  stageEnd(2);
}

// ---- pages in the app's dark theme ------------------------------------------------
static void drawBrandBar(const View &v) {
  if (!v.st.wifi || !v.st.apiOk) drawLinkWarning(v, C_TEXT, 255);
  else drawLockup(14, 31, C_BRAND, C_LOGO_SUGAR, C_BRAND);
  drawDots(C_DIM, 255);
}

// A full page that explains why there is no number (the account's state).
static void drawNotice(const View &v, const char *title, const char *l1, const char *l2, const char *l3) {
  gx::clear(C_BG);
  drawBrandBar(v);
  int y = H / 2 - 30;
  gx::textC(F_TITLE, W / 2, y, title, C_TEXT);
  if (l1) gx::textC(F_MED, W / 2, y + 40, l1, C_DIM);
  if (l2) gx::textC(F_MED, W / 2, y + 68, l2, C_DIM);
  if (l3) gx::textC(F_MED, W / 2, y + 96, l3, C_DIM);
}

static void drawQr(int x, int y, int box, const uint8_t *mods, int n);

// The paired account has no glucose source yet: usually a person who met the
// display before the app. Say the one step that is left and put the app one
// scan away. The screen needs no touch: readings start by themselves.
static void drawGetApp(const View &v) {
  gx::clear(C_BG);
  drawBrandBar(v);
  static const char *LINES[4] = {"Get the Sugar Sense app on your", "phone, sign in with the account", "you paired this display with,", "and connect your sensor."};
  const char *title = "One more step";
  const char *after = "Readings start here by themselves.";
  const bool code = v.qr && v.qrN > 0;
  if (H >= W) {
    gx::textC(F_TITLE, W / 2, 84, title, C_TEXT);
    for (int i = 0; i < 4; i++) gx::textC(F_SMALL, W / 2, 112 + i * 20, LINES[i], C_DIM);
    if (code) {
      drawQr(W / 2 - 74, 196, 148, v.qr, v.qrN);
      if (v.qrLabel) gx::textC(F_SMALL, W / 2, 368, v.qrLabel, C_DIM);
    }
    gx::textC(F_SMALL, W / 2, code ? 398 : 220, after, C_FAINT);
  } else if (code) {
    drawQr(22, 66, 148, v.qr, v.qrN);
    if (v.qrLabel) gx::text(F_SMALL, 22, 240, v.qrLabel, C_DIM);
    const int x = 196;
    gx::text(F_TITLE, x, 88, title, C_TEXT);
    for (int i = 0; i < 4; i++) gx::text(F_SMALL, x, 116 + i * 20, LINES[i], C_DIM);
    gx::text(F_SMALL, x, 214, after, C_FAINT);
  } else {
    gx::textC(F_TITLE, W / 2, H / 2 - 58, title, C_TEXT);
    for (int i = 0; i < 4; i++) gx::textC(F_SMALL, W / 2, H / 2 - 28 + i * 20, LINES[i], C_DIM);
    gx::textC(F_SMALL, W / 2, H / 2 + 70, after, C_FAINT);
  }
}

// The icon over the two-tone wordmark, with one line under it.
static void drawSplash(const char *line) {
  gx::clear(C_BG);
  int iy = H / 2 - ICON_PX / 2 - 46;
  gx::image(W / 2 - ICON_PX / 2, iy, ICON_PX, ICON_PX, ICON_RGB565);
  int w = gx::textW(F_TITLE, "Sugar Sense");
  int x = gx::text(F_TITLE, W / 2 - w / 2, iy + ICON_PX + 44, "Sugar ", C_LOGO_SUGAR);
  gx::text(F_TITLE, x, iy + ICON_PX + 44, "Sense", C_BRAND);
  if (line) gx::textC(F_SMALL, W / 2, iy + ICON_PX + 74, line, C_DIM);
}

// The home screen: the scene, or the page that says why there is none.
static void drawHome(const View &v, int look) {
  const Snapshot &s = *v.snap;
  if (s.state == ss::STATE_NONE) {
    gx::clear(C_BG);
    drawBrandBar(v);
    gx::textC(F_TITLE, W / 2, H / 2, "Connecting" CH_ELL, C_TEXT);
    return;
  }
  if (s.state == ss::STATE_UNAVAILABLE) {
    // the display is free; this answer is kept for a rule the server may add one day
    drawNotice(v, "Not available", "This account cannot show", "glucose on a display yet.", "Open the app on your phone.");
    return;
  }
  if (s.state == ss::STATE_FOLLOWER) {
    drawNotice(v, "Follower account", "This display shows your own", "glucose, and this account", "follows someone else.");
    return;
  }
  if (s.state == ss::STATE_NO_SOURCE) {
    drawGetApp(v);
    return;
  }
  if (look == LOOK_NIGHT) drawNight(v);
  else if (look == LOOK_COLOR) drawDay(v);
  else drawDark(v);
}

// ---- several people: the split screen -------------------------------------------
// One tile of the split screen: a snapshot (the account's own, or a Person
// copied into one), its name, and the two states a followed person can be in
// besides a reading.
struct TileSource {
  const Snapshot *snap;
  const char *label;
  bool self;        // the account's own glucose
  bool notShared;   // the server answered state 2 for this person
  bool paused;      // the person paused sharing
};

// The word a tile shows in the number's place ("Not shared", "Paused", "No
// Data"): the title font when it fits the tile with its margins, else the
// medium one.
static const AAFont &tileWordFont(const char *word, int tileW) {
  return gx::textW(F_TITLE, word) <= tileW - 32 ? F_TITLE : F_MED;
}

// The number for a tile: the largest size from NUM_S down whose widest value
// fits the tile's width beside its arrow and whose height leaves room for the
// name above it and at least one small line under it.
static const int TILE_NAME_H = 30, TILE_GAP = 12;   // the name's room above the number, the gap under it

static NumStyle tileNum(bool mmol, int w, int h) {
  const char *widest = mmol ? "88.8" : "888";
  for (int i = NUM_S; i < NUM_COUNT; i++) {
    const NumStyle &n = NUM_STYLES[i];
    if (gx::textW(*n.num, widest, n.track) + 10 + n.arrowPx <= w - 24 && n.cap <= h - (TILE_NAME_H + TILE_GAP + 20)) return n;
  }
  return NUM_STYLES[NUM_COUNT - 1];
}

// The split screen's header: the Night scene's clock in its strip, else the
// app's brand bar on a dark strip above the tiles (the Color scene's tiles
// carry the color, the strip stays dark so the first row is as tall as the
// second).
static const int SPLIT_TOP = 48, SPLIT_TOP_NIGHT = 78;

static void drawSplitHeader(const View &v, int look) {
  char t[8];
  if (look == LOOK_NIGHT) {
    if (!v.st.wifi || !v.st.apiOk) drawLinkWarning(v, C_DIM, 255);
    drawDots(C_DIM, 150);
    if (v.st.timeSynced) {
      clockText(v, t, sizeof(t));
      gx::textC(F_CLOCK, W / 2, 62, t, C_TEXT, 150);
    }
    return;
  }
  if (!v.st.wifi || !v.st.apiOk) drawLinkWarning(v, C_TEXT, 255);
  else drawLockup(14, 31, C_BRAND, C_LOGO_SUGAR, C_BRAND);
  drawDots(C_DIM, 255);
  if (v.st.timeSynced) {
    clockText(v, t, sizeof(t));
    gx::textR(F_MED, W - 56, 31, t, C_TEXT, 210);
  }
}

// One tile: the name, the number with its arrow, the change and the age. On
// a narrow tile the change and the age take two lines.
static void drawTile(const View &v, int look, const ss::Tile &r, const TileSource &t, int contentTop) {
  View tv = v;
  tv.snap = t.snap;
  Headline h = headline(tv);
  const bool color = look == LOOK_COLOR, night = look == LOOK_NIGHT;
  const bool live = h.fresh && !t.paused && !t.notShared;
  uint16_t ink, name, dim;
  int nameAlpha = 255, dimAlpha = 255;
  if (color) {
    gx::rect(r.x, r.y, r.w, r.h, live ? zoneColor(h.zone) : C_SLATE);
    if (r.x > 0) gx::rect(r.x, r.y, 1, r.h, C_BLACK, 70);   // a hairline between tiles
    if (r.y > SPLIT_TOP) gx::rect(r.x, r.y, r.w, 1, C_BLACK, 70);
    ink = name = dim = C_WHITE;
    nameAlpha = 220;
    dimAlpha = 190;
  } else if (night) {
    if (r.x > 0) gx::rect(r.x, r.y + 10, 1, r.h - 20, C_LINE, 120);
    if (r.y > SPLIT_TOP_NIGHT) gx::rect(r.x + 10, r.y, r.w - 20, 1, C_LINE, 120);
    ink = live ? zoneColor(h.zone) : C_NIGHT_STALE;
    name = dim = C_DIM;
  } else {
    gx::rrect(r.x + 6, r.y + 6, r.w - 12, r.h - 12, 18, C_CARD);
    ink = live ? zoneColor(h.zone) : C_SLATE;
    name = C_TEXT;
    nameAlpha = 210;
    dim = C_DIM;
  }
  const int room = (r.y + r.h) - contentTop;
  NumStyle ns = tileNum(t.snap->isMmol, r.w, room);
  // Under the number: a wide tile carries the app's change line (the change
  // bold, the unit, the age); a narrower one takes two lines when the height
  // allows, else one small line with everything on it.
  const int mode = r.w >= 300 ? 0 : (room >= TILE_NAME_H + ns.cap + TILE_GAP + 44 ? 1 : 2);
  const int linesH = mode == 0 ? 26 : (mode == 1 ? 44 : 18);
  const int block = TILE_NAME_H + ns.cap + TILE_GAP + linesH;
  int y = contentTop + (room - block) / 2;
  if (y < contentTop) y = contentTop;
  const int cx = r.x + r.w / 2;
  char cut[32];
  fitText(F_MED, t.label, r.w - 24, cut, sizeof(cut));
  gx::textC(F_MED, cx, y + 16, cut, name, nameAlpha);
  const int numBase = y + TILE_NAME_H + ns.cap, lineBase = numBase + TILE_GAP + (mode == 2 ? 14 : 22);
  const uint16_t lineInk = color ? C_WHITE : (night ? ink : C_TEXT);
  if (t.notShared || t.paused || !h.fresh) {
    const bool waiting = t.snap->state == ss::STATE_NONE;   // asked for, not answered yet
    const char *big = t.notShared ? "Not shared" : (t.paused ? "Paused" : (waiting ? CH_ELL : "No Data"));
    const char *small = t.notShared ? "Open the app" : (t.paused ? "Sharing paused" : (waiting ? "Connecting" : h.note));
    char cutSmall[40];
    fitText(F_SMALL, small, r.w - 28, cutSmall, sizeof(cutSmall));
    gx::textC(tileWordFont(big, r.w), cx, numBase - ns.cap / 2 + 10, big, ink);
    gx::textC(F_SMALL, cx, lineBase - 4, cutSmall, dim, dimAlpha);
    return;
  }
  drawNumber(cx, numBase, h, ns, ink);
  if (mode == 0) {
    drawChangeLine(cx, lineBase, h, true, lineInk, color ? 190 : (night ? 170 : 150));
    return;
  }
  char first[24];
  if (h.delta[0]) snprintf(first, sizeof(first), "%s %s", h.delta, h.unit);
  else snprintf(first, sizeof(first), "%s", h.unit);
  if (mode == 1) {
    gx::textC(F_MED, cx, lineBase - 2, first, lineInk, color ? 220 : (night ? 190 : 200));
    gx::textC(F_SMALL, cx, lineBase + 18, h.note, dim, dimAlpha);
    return;
  }
  char one[48];
  snprintf(one, sizeof(one), "%s  " CH_DOT "  %s", first, h.note);
  gx::textC(F_SMALL, cx, lineBase, one, lineInk, color ? 210 : (night ? 180 : 190));
}

// Several people at once: the screen split into tiles (ss::tileLayout), each
// in the scene's own look, the header over them or above them.
static void drawSplit(const View &v, int look, const TileSource *t, int n) {
  gx::clear(look == LOOK_NIGHT ? C_BLACK : C_BG);
  const int top = look == LOOK_NIGHT ? SPLIT_TOP_NIGHT : SPLIT_TOP;
  ss::Tile tiles[SS_MAX_TILES];
  const int count = ss::tileLayout(n, W, H, top, tiles);
  for (int i = 0; i < count; i++) drawTile(v, look, tiles[i], t[i], tiles[i].y + 10);
  drawSplitHeader(v, look);
}

static void drawHeader(const char *title, const char *sub) {
  gx::clear(C_BG);
  gx::text(F_TITLE, 14, 37, title, C_TEXT);
  if (sub) gx::textR(F_SMALL, W - 14, 34, sub, C_DIM);
}

// A list of rows with two buttons under it: Settings, the network picker.
struct MenuLayout {
  int top, rowH, gap, maxRows;
  Rect row(int i) const { return {10, top + i * (rowH + gap), W - 20, rowH}; }
  Rect minus(int i) const {
    Rect r = row(i);
    return {r.x + r.w - 86, r.y, 42, r.h};
  }
  Rect plus(int i) const {
    Rect r = row(i);
    return {r.x + r.w - 44, r.y, 44, r.h};
  }
  Rect left() const { return {10, H - 56, (W - 30) / 2, 46}; }
  Rect right() const { return {20 + (W - 30) / 2, H - 56, (W - 30) / 2, 46}; }
  // Settings: the page names as tabs in the title's place, side by side
  Rect tab(int i, int n) const {
    const int x0 = 10, w = W - 20, tw = w / n;
    return {x0 + i * tw, 8, i == n - 1 ? w - i * tw : tw, top - 16};
  }
  // a tab answers a touch up to the screen's edge and down to the first row
  Rect tabHit(int i, int n) const {
    Rect r = tab(i, n);
    return {r.x, 0, r.w, top - 4};
  }
};

static MenuLayout menuLayout() {
  MenuLayout L;
  if (H >= W) {
    L.top = 56;
    L.rowH = 48;
    L.gap = 8;
    L.maxRows = 6;
  } else {
    L.top = 50;
    L.rowH = 38;
    L.gap = 5;
    L.maxRows = 5;
  }
  return L;
}

enum { ROW_STEPPER = 0, ROW_CYCLE = 1, ROW_ACTION = 2 };

struct Row {
  const char *label;
  char value[36];
  int kind;
};

static void drawRows(const Row *rows, int n) {
  MenuLayout L = menuLayout();
  for (int i = 0; i < n && i < L.maxRows; i++) {
    Rect r = L.row(i);
    gx::rrect(r.x, r.y, r.w, r.h, 16, C_CARD);
    int base = r.y + r.h / 2 + 8;
    int labelEnd = gx::text(F_MED, r.x + 16, base, rows[i].label, C_TEXT);
    if (rows[i].kind == ROW_STEPPER) {
      Rect m = L.minus(i), p = L.plus(i);
      int d = r.h - 12;   // two round buttons
      gx::rrect(m.x + (m.w - d) / 2, m.y + 6, d, d, d / 2, C_RAISED);
      gx::textC(F_MED, m.x + m.w / 2, base, CH_MINUS, C_TEXT);
      gx::rrect(p.x + (p.w - d) / 2 - 2, p.y + 6, d, d, d / 2, C_RAISED);
      gx::textC(F_MED, p.x + p.w / 2 - 2, base, "+", C_TEXT);
      gx::textR(F_MED, m.x - 6, base, rows[i].value, C_DIM);
    } else if (rows[i].kind == ROW_CYCLE) {
      // a value never runs into its label, on the narrow screen either
      char cut[40];
      fitText(F_MED, rows[i].value, r.x + r.w - 16 - labelEnd - 12, cut, sizeof(cut));
      gx::textR(F_MED, r.x + r.w - 16, base, cut, C_BRAND);
    } else {
      gx::textR(F_MED, r.x + r.w - 16, base, ">", C_FAINT);
      if (rows[i].value[0]) {
        char cut[40];
        fitText(F_MED, rows[i].value, r.x + r.w - 44 - labelEnd - 12, cut, sizeof(cut));
        gx::textR(F_MED, r.x + r.w - 36, base, cut, C_DIM);
      }
    }
  }
}

static void drawMenu(const char *title, const char *sub, const Row *rows, int n, const char *leftLabel, const char *rightLabel) {
  drawHeader(title, sub);
  drawRows(rows, n);
  MenuLayout L = menuLayout();
  if (leftLabel) drawButton(L.left(), leftLabel, false);
  if (rightLabel) drawButton(L.right(), rightLabel, false);
}

// The tabs' font: the medium one when every page name fits its tab, else the
// small one (four tabs on the upright screen).
static const AAFont &tabFont(const char *const *tabs, int n) {
  MenuLayout L = menuLayout();
  for (int i = 0; i < n; i++)
    if (gx::textW(F_MED, tabs[i]) > L.tab(i, n).w - 14) return F_SMALL;
  return F_MED;
}

// Settings: every page named as a tab on top, the current one filled in the
// brand green, so the pages show they are pages and any is one tap away; the
// rows; Done bottom right on every page; a quiet note bottom left (the
// version, a page's hint). (2026-11-06, the owner: nothing said the old
// "next page" button switched the page.)
static void drawSettings(const char *const *tabs, int nt, int cur, const char *note, const Row *rows, int n) {
  gx::clear(C_BG);
  MenuLayout L = menuLayout();
  const int th = L.top - 16;
  gx::rrect(10, 8, W - 20, th, th / 2, C_CARD);
  const AAFont &f = tabFont(tabs, nt);
  const int lift = &f == &F_MED ? 8 : 6;
  for (int i = 0; i < nt; i++) {
    Rect r = L.tab(i, nt);
    if (i == cur) gx::rrect(r.x + 3, r.y + 3, r.w - 6, r.h - 6, (r.h - 6) / 2, C_BRAND);
    gx::textC(f, r.x + r.w / 2, r.y + r.h / 2 + lift, tabs[i], i == cur ? C_ON_BRAND : C_DIM);
  }
  drawRows(rows, n);
  drawButton(L.right(), "Done", false);
  if (note) gx::text(F_SMALL, 16, L.right().y + L.right().h / 2 + 6, note, C_DIM);
}

static void drawWifiList(const char *const *ssids, const int *rssi, int n, const char *sub, const char *leftLabel) {
  drawHeader("Wi-Fi", sub);
  MenuLayout L = menuLayout();
  if (n == 0) gx::textC(F_MED, W / 2, H / 2, "No networks found", C_DIM);
  for (int i = 0; i < n && i < L.maxRows; i++) {
    Rect r = L.row(i);
    gx::rrect(r.x, r.y, r.w, r.h, 16, C_CARD);
    int base = r.y + r.h / 2 + 8;
    char cut[40];
    fitText(F_MED, ssids[i], r.w - 16 - 60, cut, sizeof(cut));
    gx::text(F_MED, r.x + 16, base, cut, C_TEXT);
    int bars = rssi[i] >= -55 ? 4 : (rssi[i] >= -67 ? 3 : (rssi[i] >= -78 ? 2 : 1));
    for (int b = 0; b < 4; b++) {
      int bh = 5 + b * 4;
      gx::rrect(r.x + r.w - 48 + b * 8, r.y + r.h / 2 + 9 - bh, 5, bh, 2, b < bars ? C_BRAND : C_LINE);
    }
  }
  if (leftLabel) drawButton(L.left(), leftLabel, false);
  drawButton(L.right(), "Scan again", false);
}

// A message in the middle of a titled page (the network picker's states).
static void drawPageNote(const char *title, const char *l1, const char *l2, uint16_t c2) {
  drawHeader(title, nullptr);
  gx::textC(F_MED, W / 2, H / 2 - 14, l1, C_DIM);
  if (l2) {
    char cut[48];
    fitText(F_MED, l2, W - 40, cut, sizeof(cut));
    gx::textC(F_MED, W / 2, H / 2 + 16, cut, c2);
  }
}

// ---- the on-screen keyboard --------------------------------------------------------
// One table of keys per layer, built for the current screen size; the drawing
// and the hit test both walk it. Layers: 0 lower case, 1 upper case, 2 digits
// and common symbols, 3 the rest of the symbols.
enum { KEY_CHAR = 0, KEY_SHIFT, KEY_LAYER, KEY_BACKSPACE, KEY_SPACE, KEY_CANCEL, KEY_OK };

struct Key {
  int16_t x, y, w, h;
  uint8_t kind;
  char ch;   // KEY_CHAR only
};

#define KB_MAX_KEYS 40

static int kbBuild(int layer, Key *keys) {
  static const char *const ROWS[4][3] = {
      {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
      {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
      {"1234567890", "@#$&*-+=_", ".,!?/()"},
      {"[]{}<>^~|\\", "%\"';:`,.?", "!@#$&*-"},
  };
  const int unit = W / 10;                       // one letter key
  const int kh = (H >= W) ? 52 : 44;
  const int y0 = H - 4 * kh - 4;
  int n = 0;
  for (int row = 0; row < 3; row++) {
    const char *chars = ROWS[layer & 3][row];
    int len = (int)strlen(chars);
    int x = (W - len * unit) / 2;                // each row centred
    for (int i = 0; i < len; i++) keys[n++] = {(int16_t)(x + i * unit), (int16_t)(y0 + row * kh), (int16_t)unit, (int16_t)kh, KEY_CHAR, chars[i]};
    if (row == 2) {                              // shift and backspace take the two ends
      keys[n++] = {0, (int16_t)(y0 + row * kh), (int16_t)x, (int16_t)kh, KEY_SHIFT, 0};
      keys[n++] = {(int16_t)(x + len * unit), (int16_t)(y0 + row * kh), (int16_t)(W - x - len * unit), (int16_t)kh, KEY_BACKSPACE, 0};
    }
  }
  const int y3 = y0 + 3 * kh;
  keys[n++] = {0, (int16_t)y3, (int16_t)(2 * unit), (int16_t)kh, KEY_LAYER, 0};
  keys[n++] = {(int16_t)(2 * unit), (int16_t)y3, (int16_t)(4 * unit), (int16_t)kh, KEY_SPACE, 0};
  keys[n++] = {(int16_t)(6 * unit), (int16_t)y3, (int16_t)(2 * unit), (int16_t)kh, KEY_CANCEL, 0};
  keys[n++] = {(int16_t)(8 * unit), (int16_t)y3, (int16_t)(W - 8 * unit), (int16_t)kh, KEY_OK, 0};
  return n;
}

// The key under a tap, or null.
static const Key *kbHit(int sx, int sy, const Key *keys, int n) {
  for (int i = 0; i < n; i++)
    if (sx >= keys[i].x && sx < keys[i].x + keys[i].w && sy >= keys[i].y && sy < keys[i].y + keys[i].h) return &keys[i];
  return nullptr;
}

// `shown` is what the field displays (the caller masks a password).
static void drawKeyboard(const char *title, const char *shown, int layer) {
  gx::clear(C_BG);
  gx::text(F_SMALL, 16, 28, title, C_DIM);
  gx::rrect(10, 40, W - 20, 46, 16, C_CARD);
  const char *p = shown;   // keep the tail visible when the text is too long
  while (*p && gx::textW(F_MED, p) > W - 20 - 44) p++;
  int x = gx::text(F_MED, 26, 71, p, C_TEXT);
  gx::rect(x + 2, 52, 2, 23, C_BRAND);

  Key keys[KB_MAX_KEYS];
  int n = kbBuild(layer, keys);
  for (int i = 0; i < n; i++) {
    const Key &k = keys[i];
    char one[2] = {k.ch, 0};
    const char *label = one;
    uint16_t bg = C_RAISED, fg = C_TEXT;
    switch (k.kind) {
      case KEY_SHIFT:
        label = layer == 0 ? "ABC" : (layer == 1 ? "abc" : (layer == 2 ? "#+=" : "123"));
        bg = layer == 1 ? C_BRAND : C_CARD;
        fg = layer == 1 ? C_ON_BRAND : C_DIM;
        break;
      case KEY_LAYER: label = layer < 2 ? "123" : "abc"; bg = C_CARD; fg = C_DIM; break;
      case KEY_BACKSPACE: label = "<"; bg = C_CARD; fg = C_DIM; break;
      case KEY_SPACE: label = "space"; fg = C_DIM; break;
      case KEY_CANCEL: label = "Back"; bg = C_CARD; fg = C_DIM; break;
      case KEY_OK: label = "OK"; bg = C_BRAND; fg = C_ON_BRAND; break;
      default: break;
    }
    gx::rrect(k.x + 2, k.y + 2, k.w - 4, k.h - 4, 9, bg);
    gx::textC(F_MED, k.x + k.w / 2, k.y + k.h / 2 + 8, label, fg);
  }
}

// ---- pairing ----------------------------------------------------------------
// `mods` = the QR code, one byte per module, n x n; null draws no code.
static void drawQr(int x, int y, int box, const uint8_t *mods, int n) {
  gx::rrect(x, y, box, box, 14, C_WHITE);
  if (!mods || n <= 0) return;
  int m = (box - 16) / n;
  if (m < 1) m = 1;
  int off = (box - m * n) / 2;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++)
      if (mods[j * n + i]) gx::rect(x + off + i * m, y + off + j * m, m, m, C_BLACK);
}

// `url` is the pair page as a person would type it (no scheme).
static void drawPairing(const View &v, const char *code, int secondsLeft, const uint8_t *qr, int qrN, const char *url, const char *note) {
  gx::clear(C_BG);
  drawBrandBar(v);
  char left[32] = "";
  if (secondsLeft >= 0) snprintf(left, sizeof(left), "Code expires in %d:%02d", secondsLeft / 60, secondsLeft % 60);
  const char *title = "Pair this display";
  if (!code || !code[0]) {
    // no code yet (the first request is on its way, or the server is unreachable)
    gx::textC(F_TITLE, W / 2, H / 2 - 20, title, C_TEXT);
    gx::textC(F_MED, W / 2, H / 2 + 20, note ? note : "Getting a code" CH_ELL, note ? C_WARN : C_DIM);
    return;
  }
  // The three lines say the whole flow, and that the account is the app's:
  // a person who has no app yet learns it here, before the sign-in.
  static const char *STEPS[3] = {"Scan with your phone and sign in", "with your Sugar Sense app account,", "then confirm this code:"};
  if (H >= W) {
    gx::textC(F_TITLE, W / 2, 84, title, C_TEXT);
    for (int i = 0; i < 3; i++) gx::textC(F_SMALL, W / 2, 110 + i * 20, STEPS[i], C_DIM);
    gx::rrect(40, 164, W - 80, 66, 16, C_CARD);
    gx::textC(F_CODE, W / 2, 214, code, C_TEXT);
    drawQr(W / 2 - 74, 244, 148, qr, qrN);
    gx::textC(F_SMALL, W / 2, 412, url, C_DIM);
    gx::textC(F_SMALL, W / 2, 432, left, C_FAINT);
    if (note) gx::textC(F_SMALL, W / 2, 456, note, C_WARN);
  } else {
    drawQr(22, 66, 148, qr, qrN);
    gx::text(F_SMALL, 22, 240, "or open in a browser:", C_FAINT);
    gx::text(F_SMALL, 22, 262, url, C_DIM);
    int x = 196;
    gx::text(F_TITLE, x, 88, title, C_TEXT);
    for (int i = 0; i < 3; i++) gx::text(F_SMALL, x, 112 + i * 20, STEPS[i], C_DIM);
    gx::rrect(x, 166, W - x - 16, 62, 16, C_CARD);
    gx::textC(F_CODE, x + (W - x - 16) / 2, 214, code, C_TEXT);
    gx::textR(F_SMALL, W - 16, 300, left, C_FAINT);
    if (note) gx::text(F_SMALL, 22, 300, note, C_WARN);
  }
}

// "Update available": where the new version is and how to install it without
// losing the display's Wi-Fi and pairing. One button; any tap goes back.
static void drawUpdate(const char *version, const char *where) {
  char sub[24];
  snprintf(sub, sizeof(sub), "v%s", version);
  drawHeader("Update available", sub);
  const int y = H >= W ? 150 : 96;
  gx::textC(F_MED, W / 2, y, "On a computer, open", C_DIM);
  gx::textC(F_MED, W / 2, y + 32, where, C_BRAND);
  gx::textC(F_SMALL, W / 2, y + 70, "Plug this display in and click Install.", C_DIM);
  gx::textC(F_SMALL, W / 2, y + 92, "Leave \"Erase device\" unticked to keep", C_DIM);
  gx::textC(F_SMALL, W / 2, y + 112, "your Wi-Fi and your pairing.", C_DIM);
  MenuLayout L = menuLayout();
  Rect b = L.left();
  b.x = W / 2 - b.w / 2;
  drawButton(b, "Done", false);
}

// A question with a quiet "no" and a red "yes" (a destructive answer).
static void drawConfirm(const char *title, const char *l1, const char *l2, const char *l3, const char *noLabel, const char *yesLabel) {
  drawHeader(title, nullptr);
  if (l1) gx::textC(F_MED, W / 2, H / 2 - 34, l1, C_DIM);
  if (l2) gx::textC(F_MED, W / 2, H / 2 - 6, l2, C_DIM);
  if (l3) gx::textC(F_MED, W / 2, H / 2 + 22, l3, C_DIM);
  MenuLayout L = menuLayout();
  drawButton(L.left(), noLabel, false);
  Rect y = L.right();
  gx::rrect(y.x, y.y, y.w, y.h, y.h / 2, C_ALERT);
  gx::textC(F_MED, y.x + y.w / 2, y.y + y.h / 2 + 8, yesLabel, C_BLACK);
}

}  // namespace ui

#endif
