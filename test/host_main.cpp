// Host harness for the display firmware's pure code (ss_logic.h, gx.h, ui.h).
// Built and driven by tools/host_test.py on a computer:
//
//   host logic                       the display-rule assertions
//   host tail <mmol> <gapSec> <prevMgdl> <newMgdl>
//                                    the change text for one tail pair
//   host render <dir>                every screen, both orientations, as raw
//                                    RGB565 files named <scene>_<W>x<H>.raw
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "../firmware/sugarsense_display/ui.h"

static uint16_t FB[gx::NATIVE_W * gx::NATIVE_H];
static int failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      failures++;                                                \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                            \
  } while (0)

static std::string value(double mgdl, bool mmol) {
  char b[8];
  ss::formatValue(b, sizeof(b), mgdl, mmol);
  return b;
}

static std::string delta(double d, bool mmol) {
  char b[16];
  ss::formatDelta(b, sizeof(b), d, mmol, "\xE2\x88\x92");
  return b;
}

static int runLogic() {
  const ss::Band b = ss::DEFAULT_BAND;
  // zones: the limits themselves are OUT of range
  CHECK(ss::zoneFor(100, b) == ss::ZONE_IN_RANGE);
  CHECK(ss::zoneFor(71, b) == ss::ZONE_IN_RANGE);
  CHECK(ss::zoneFor(70, b) == ss::ZONE_WARNING);
  CHECK(ss::zoneFor(56, b) == ss::ZONE_WARNING);
  CHECK(ss::zoneFor(55, b) == ss::ZONE_ALERT);
  CHECK(ss::zoneFor(179, b) == ss::ZONE_IN_RANGE);
  CHECK(ss::zoneFor(180, b) == ss::ZONE_WARNING);
  CHECK(ss::zoneFor(249, b) == ss::ZONE_WARNING);
  CHECK(ss::zoneFor(250, b) == ss::ZONE_ALERT);
  // staleness: strictly more than 15 minutes / 2 hours
  const int64_t now = 1800000000000LL;
  CHECK(!ss::isStale(now, now - 900000));
  CHECK(ss::isStale(now, now - 900001));
  CHECK(ss::isStale(now, 0));
  CHECK(!ss::isLongStale(now, now - 7200000));
  CHECK(ss::isLongStale(now, now - 7200001));
  // units
  CHECK(value(100, true) == "5.6");
  CHECK(value(70, true) == "3.9");
  CHECK(value(180, true) == "10.0");
  CHECK(value(250, true) == "13.9");
  CHECK(value(104, false) == "104");
  CHECK(value(104.5, false) == "105");
  CHECK(value(104.4, false) == "104");
  // arrows
  CHECK(ss::clampArrow(0) == 3);
  CHECK(ss::clampArrow(6) == 3);
  CHECK(ss::clampArrow(1) == 1);
  CHECK(ss::clampArrow(5) == 5);
  // age
  CHECK(ss::elapsedMinutes(now, now - 59000) == 0);
  CHECK(ss::elapsedMinutes(now, now - 23 * 60000 - 40000) == 23);
  CHECK(ss::elapsedMinutes(now, now + 5000) == 0);
  CHECK(ss::elapsedMinutes(now, 0) == -1);
  // change text
  CHECK(delta(0, false) == "0");
  CHECK(delta(5, false) == "+5");
  CHECK(delta(-4, false) == "\xE2\x88\x92" "4");
  CHECK(delta(0, true) == "0.0");
  CHECK(delta(1, true) == "+0.1");
  CHECK(delta(-5, true) == "\xE2\x88\x92" "0.3");
  CHECK(delta(1.5, false) == "+2");
  CHECK(delta(-1.5, false) == "\xE2\x88\x92" "2");
  CHECK(delta(0.4, false) == "0");
  // the tail guards
  double d = 0;
  ss::Point two[2] = {{now - 300000, 110}, {now, 116}};
  CHECK(ss::deltaFromPoints(two, 2, now, now, &d) && d == 6);
  CHECK(!ss::deltaFromPoints(two, 1, now, now, &d));
  CHECK(!ss::deltaFromPoints(two, 2, now - 60000, now, &d));            // the tail is not the headline
  CHECK(!ss::deltaFromPoints(two, 2, now, now + 16 * 60000, &d));       // a stale headline
  ss::Point gap[2] = {{now - 1200001, 110}, {now, 116}};
  CHECK(!ss::deltaFromPoints(gap, 2, now, now, &d));                    // past the 20 minute cap
  ss::Point edge[2] = {{now - 1200000, 110}, {now, 116}};
  CHECK(ss::deltaFromPoints(edge, 2, now, now, &d));                    // the cap is inclusive
  // local time and the night window
  int hh = 0, mm = 0;
  ss::localHourMinute(1800000000000LL, 240, &hh, &mm);   // 2027-01-15 08:00:00 UTC
  CHECK(hh == 12 && mm == 0);
  ss::localHourMinute(1800000000000LL, -210, &hh, &mm);
  CHECK(hh == 4 && mm == 30);
  CHECK(ss::isNightHour(23, 23, 7) && ss::isNightHour(3, 23, 7) && !ss::isNightHour(7, 23, 7) && !ss::isNightHour(12, 23, 7));
  CHECK(ss::isNightHour(2, 1, 5) && !ss::isNightHour(5, 1, 5) && !ss::isNightHour(9, 9, 9));

  // the look for a Scene setting: Auto is Color by day and Night at night, the
  // three named scenes hold at every hour, an unknown value reads as Auto
  CHECK(ss::lookForScene(0, false) == 2 && ss::lookForScene(0, true) == 1);
  CHECK(ss::lookForScene(1, false) == 0 && ss::lookForScene(1, true) == 0);
  CHECK(ss::lookForScene(2, false) == 1 && ss::lookForScene(2, true) == 1);
  CHECK(ss::lookForScene(3, false) == 2 && ss::lookForScene(3, true) == 2);
  CHECK(ss::lookForScene(9, false) == 2 && ss::lookForScene(9, true) == 1);

  // which firmware version is newer
  CHECK(ss::compareVersions("0.3.2", "0.3.1") > 0 && ss::compareVersions("0.3.1", "0.3.2") < 0);
  CHECK(ss::compareVersions("0.3.1", "0.3.1") == 0 && ss::compareVersions("0.3", "0.3.0") == 0);
  CHECK(ss::compareVersions("0.10.0", "0.9.3") > 0 && ss::compareVersions("1.0.0", "0.99.99") > 0);
  CHECK(ss::compareVersions("0.3.1.1", "0.3.1") > 0 && ss::compareVersions("0.4", "0.3.9") > 0);
  CHECK(ss::compareVersions("0.3.2-beta", "0.3.2") == 0 && ss::compareVersions("", "0.3.1") < 0 && ss::compareVersions("", "") == 0);
  // the clock from an HTTP Date header
  int64_t at = 0;
  CHECK(ss::parseHttpDate("Thu, 05 Nov 2026 07:22:46 GMT", &at) && at == 1793863366LL);
  CHECK(ss::parseHttpDate("Thu, 29 Feb 2024 23:59:59 GMT", &at) && at == 1709251199LL);
  CHECK(ss::parseHttpDate("Sat, 01 Mar 2025 00:00:00 GMT", &at) && at == 1740787200LL);
  CHECK(!ss::parseHttpDate("yesterday", &at));
  CHECK(!ss::parseHttpDate("Thu, 05 Foo 2026 07:22:46 GMT", &at));
  CHECK(!ss::parseHttpDate(nullptr, &at));
  // the touch mapping is the inverse of the pixel mapping, in every rotation
  gx::fb = FB;
  for (int r = 0; r < 4; r++) {
    gx::setRotation(r);
    int x = 17, y = 29;
    uint16_t *cell = gx::at(x, y);
    int idx = (int)(cell - FB), nx = idx % gx::NATIVE_W, ny = idx / gx::NATIVE_W, bx = -1, by = -1;
    gx::fromNative(nx, ny, &bx, &by);
    CHECK(bx == x && by == y);
  }
  // the smooth curve passes through its points and never leaves the range between two neighbours
  {
    ui::Curve c;
    const float ys[7] = {40, 40, 10, 90, 88, 30, 30};
    for (int i = 0; i < 7; i++) c.add(10.0f + i * 20, ys[i]);
    c.finish();
    for (int i = 0; i < 7; i++) CHECK(fabsf(c.at(10.0f + i * 20) - ys[i]) < 0.01f);
    for (int i = 0; i < 6; i++) {
      float lo = ys[i] < ys[i + 1] ? ys[i] : ys[i + 1], hi = ys[i] > ys[i + 1] ? ys[i] : ys[i + 1];
      for (float x = 10.0f + i * 20; x <= 30.0f + i * 20; x += 0.5f) CHECK(c.at(x) >= lo - 0.01f && c.at(x) <= hi + 0.01f);
    }
    c.add(5.0f, 1.0f);   // a point that does not move right is refused
    CHECK(c.n == 7);
  }
  // the line's color is the zone color away from a limit
  CHECK(ui::curveColor(120, b) == ui::C_OK && ui::curveColor(210, b) == ui::C_WARN && ui::curveColor(300, b) == ui::C_ALERT);
  CHECK(ui::curveColor(62, b) == ui::C_WARN && ui::curveColor(40, b) == ui::C_ALERT);
  // outside text never reaches the fonts' private slots
  char lab[40];
  ui::asciiLabel("Anna\xE2\x80\x99s Wifi", lab, sizeof(lab));
  CHECK(std::string(lab) == "Anna's Wifi");
  ui::asciiLabel("\xE1\x83\x98\xE1\x83\xA0 5G", lab, sizeof(lab));
  CHECK(std::string(lab) == "?? 5G");
  ui::asciiLabel("Caf\xC3\xA9", lab, sizeof(lab));
  CHECK(std::string(lab) == "Caf?");
  // the keyboard: every key of every layer answers a tap at its own centre,
  // the keys tile the rows without a hole, and the widest value fits its font
  for (int r = 0; r < 2; r++) {
    gx::setRotation(r);
    for (int layer = 0; layer < 4; layer++) {
      ui::Key keys[KB_MAX_KEYS];
      int n = ui::kbBuild(layer, keys);
      CHECK(n > 30 && n <= KB_MAX_KEYS);
      int area = 0;
      for (int i = 0; i < n; i++) {
        const ui::Key *hit = ui::kbHit(keys[i].x + keys[i].w / 2, keys[i].y + keys[i].h / 2, keys, n);
        CHECK(hit == &keys[i]);
        area += keys[i].w * keys[i].h;
      }
      // rows of 10 and 9 letter keys, then two rows that span the whole width
      CHECK(area == (19 * (gx::W / 10) + 2 * gx::W) * keys[0].h);
    }
    for (int k = 0; k < 4; k++) {
      // the Dark scene, with and without the graph: the number and its arrow fit, the rows stack
      const bool mmol = k & 1, graph = k & 2;
      ui::DarkLayout D = ui::darkLayout(mmol, graph);
      const int numW = gx::textW(*D.ns.num, mmol ? "88.8" : "888", D.ns.track);
      CHECK(numW + 10 + D.arrowPx <= (D.side ? 316 : gx::W - 24));
      CHECK(D.numBase - D.ns.cap >= 50);
      if (graph) {
        CHECK(D.chart.y >= D.numBase + 20 && D.chart.h >= 110 && D.chart.y + D.chart.h <= gx::H - 8);
      } else {
        CHECK(D.lineBase <= gx::H - 12 && D.chart.h == 0);
      }
    }
    for (int k = 0; k < 8; k++) {
      // the Color and Night scenes, with and without the graph
      const bool mmol = k & 1, night = k & 2, graph = k & 4;
      ui::NumStyle ns = ui::numStyle(mmol, night, graph);
      CHECK(gx::textW(*ns.num, mmol ? "88.8" : "888", ns.track) + 10 + ns.arrowPx <= gx::W - 20);
      ui::GlanceLayout L = ui::glanceLayout(night, ns.cap, graph);
      CHECK(L.numBase - ns.cap >= (night ? L.clockBase + 12 : 44));
      CHECK(!night || L.clockBase - 42 >= 4);
      const int lastLine = L.noteBase ? L.noteBase : L.lineBase;
      if (graph) {
        CHECK(L.trend.y >= lastLine + 10 && L.trend.h >= 70 && L.trend.y + L.trend.h <= gx::H - 8);
      } else {
        CHECK(lastLine <= gx::H - 12 && L.trend.h == 0);
      }
    }
  }
  printf(failures ? "logic: %d FAILED\n" : "logic: ok\n", failures);
  return failures ? 1 : 0;
}

// ---- rendering -------------------------------------------------------------
static const int64_t NOW = 1800000000000LL + 11 * 3600000LL + 41 * 60000LL;   // 23:41 at UTC+4

static void series(ui::Snapshot &s, int hours, int ageMin, int base, int swing, bool mmol) {
  s.state = ss::STATE_OK;
  s.isMmol = mmol;
  s.band = ss::DEFAULT_BAND;
  const int stepMin = hours <= 3 ? 5 : (hours == 6 ? 10 : (hours == 12 ? 15 : 30));
  const int64_t newest = NOW - (int64_t)ageMin * 60000;
  int n = hours * 60 / stepMin;
  if (n > SS_MAX_POINTS - 2) n = SS_MAX_POINTS - 2;
  s.npts = 0;
  for (int i = n; i >= 1; i--) {
    double t = (double)i / n;
    int mg = base + (int)(swing * sin(t * 7.0) + swing * 0.35 * sin(t * 19.0));
    s.pts[s.npts++] = {newest - (int64_t)i * stepMin * 60000, (double)mg};
  }
  s.pts[s.npts++] = {newest, (double)(base + 5)};
  s.hasValue = true;
  s.mgdl = base + 5;
  s.dateMs = newest;
  // The arrow agrees with the last step unless a scene sets its own.
  const double step = s.pts[s.npts - 1].mgdl - s.pts[s.npts - 2].mgdl;
  s.arrow = step > 12 ? 5 : step > 2 ? 4 : step < -12 ? 1 : step < -2 ? 2 : 3;
}

static void dump(const std::string &dir, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s_%dx%d.raw", dir.c_str(), name, gx::W, gx::H);
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    exit(1);
  }
  for (int y = 0; y < gx::H; y++)
    for (int x = 0; x < gx::W; x++) {
      uint16_t c = *gx::at(x, y);
      fwrite(&c, 2, 1, f);
    }
  fclose(f);
}

static int runRender(const std::string &dir) {
  gx::fb = FB;
  const char *url = "app.sugarsense.io/display";
  for (int r = 0; r < 2; r++) {
    gx::setRotation(r);
    const char *o = r ? "L" : "P";
    char name[64];
    ui::Snapshot s;
    ui::View v;
    v.snap = &s;
    v.nowMs = NOW;
    v.tzMin = 240;
    v.graphHours = 3;

#define SHOT(label, draw)                            \
  do {                                               \
    draw;                                            \
    snprintf(name, sizeof(name), "%s_%s", o, label); \
    dump(dir, name);                                 \
  } while (0)

    series(s, 3, 2, 104, 26, false);
    SHOT("01_dark_inrange", ui::drawHome(v, ui::LOOK_DARK));

    series(s, 6, 0, 205, 48, true);
    s.arrow = 5;
    v.graphHours = 6;
    SHOT("02_dark_high_mmol", ui::drawHome(v, ui::LOOK_DARK));

    series(s, 24, 4, 62, 40, false);
    s.mgdl = 52;
    s.pts[s.npts - 1].mgdl = 52;
    s.pts[s.npts - 2].mgdl = 60;
    s.arrow = 1;
    v.graphHours = 24;
    SHOT("03_dark_verylow_24h", ui::drawHome(v, ui::LOOK_DARK));

    series(s, 3, 23, 118, 20, false);
    v.graphHours = 3;
    SHOT("04_dark_stale", ui::drawHome(v, ui::LOOK_DARK));

    v.st.wifi = false;
    series(s, 3, 140, 118, 20, false);
    SHOT("05_dark_longstale_nowifi", ui::drawHome(v, ui::LOOK_DARK));
    v.st.wifi = true;

    s = ui::Snapshot();
    SHOT("06_connecting", ui::drawHome(v, ui::LOOK_DARK));
    s.state = ss::STATE_UNAVAILABLE;
    SHOT("07_state_unavailable", ui::drawHome(v, ui::LOOK_DARK));
    s.state = ss::STATE_NO_SOURCE;
    SHOT("08_state_nosource_plain", ui::drawHome(v, ui::LOOK_DARK));
    {
      // the usual look: the get-the-app code beside the one step that is left
      static uint8_t appQr[25 * 25];
      uint32_t seed = 777;
      for (int i = 0; i < 25 * 25; i++) {
        seed = seed * 1664525u + 1013904223u;
        appQr[i] = (seed >> 24) & 1;
      }
      v.qr = appQr;
      v.qrN = 25;
      v.qrLabel = "sugarsense.io/download";
      SHOT("08_state_nosource", ui::drawHome(v, ui::LOOK_DARK));
      v.qr = nullptr;
      v.qrN = 0;
      v.qrLabel = nullptr;
    }

    series(s, 3, 3, 96, 18, false);
    s.arrow = 3;
    SHOT("09_night_inrange", ui::drawHome(v, ui::LOOK_NIGHT));
    series(s, 3, 1, 262, 30, true);
    SHOT("10_night_high_mmol", ui::drawHome(v, ui::LOOK_NIGHT));
    series(s, 3, 31, 96, 18, false);
    SHOT("11_night_stale", ui::drawHome(v, ui::LOOK_NIGHT));

    series(s, 3, 2, 104, 26, false);
    SHOT("21_color_inrange", ui::drawHome(v, ui::LOOK_COLOR));
    series(s, 6, 0, 205, 48, true);
    v.graphHours = 6;
    SHOT("22_color_high_mmol", ui::drawHome(v, ui::LOOK_COLOR));
    series(s, 3, 23, 118, 20, false);
    v.graphHours = 3;
    SHOT("23_color_stale", ui::drawHome(v, ui::LOOK_COLOR));
    // a day that crosses every zone, for the multi-colored line
    series(s, 12, 1, 150, 110, false);
    s.arrow = 2;
    v.graphHours = 12;
    SHOT("24_dark_wide_swing", ui::drawHome(v, ui::LOOK_DARK));
    v.graphHours = 3;

    // numbers only: no graph
    v.showGraph = false;
    series(s, 3, 2, 104, 26, false);
    SHOT("31_nograph_dark_inrange", ui::drawHome(v, ui::LOOK_DARK));
    series(s, 3, 0, 205, 48, true);
    s.arrow = 5;
    SHOT("32_nograph_dark_high_mmol", ui::drawHome(v, ui::LOOK_DARK));
    series(s, 3, 23, 118, 20, false);
    SHOT("33_nograph_dark_stale", ui::drawHome(v, ui::LOOK_DARK));
    series(s, 3, 3, 62, 18, false);
    s.arrow = 2;
    SHOT("34_nograph_night", ui::drawHome(v, ui::LOOK_NIGHT));
    series(s, 3, 1, 262, 30, false);
    SHOT("35_nograph_color", ui::drawHome(v, ui::LOOK_COLOR));
    v.showGraph = true;

    SHOT("12_splash", ui::drawSplash("Connecting to Wi-Fi" CH_ELL));

    const char *ssids[] = {"Home 5G", "Fiber-4F2A", "Guest_Network_With_A_Long_Name", "TP-Link_88C1", "DIRECT-printer", "Cafe", "Neighbour"};
    const int rssi[] = {-48, -61, -70, -75, -80, -84, -90};
    SHOT("13_wifi", ui::drawWifiList(ssids, rssi, 7, "Choose your network", "Rotate"));

    SHOT("14_keyboard_abc", ui::drawKeyboard("Password for Home 5G", "*********k", 0));
    SHOT("15_keyboard_123", ui::drawKeyboard("Password for Home 5G", "", 2));

    static uint8_t qr[29 * 29];
    uint32_t seed = 12345;
    for (int i = 0; i < 29 * 29; i++) {
      seed = seed * 1664525u + 1013904223u;
      qr[i] = (seed >> 24) & 1;
    }
    ui::Snapshot none;
    v.snap = &none;
    SHOT("16_pairing", ui::drawPairing(v, "7KQ2XM", 581, qr, 29, url, nullptr));
    v.st.apiOk = false;
    SHOT("17_pairing_offline", ui::drawPairing(v, "", -1, nullptr, 0, url, "Can't reach Sugar Sense" CH_ELL));
    v.st.apiOk = true;
    v.snap = &s;

    ui::Row disp[5] = {{"Brightness", "100%", ui::ROW_STEPPER}, {"Scene", "Auto", ui::ROW_CYCLE}, {"Graph", "3 hours", ui::ROW_CYCLE}, {"Rotate screen", "", ui::ROW_ACTION}, {"Sound", "On", ui::ROW_CYCLE}};
    SHOT("18_settings_display", ui::drawMenu("Display", nullptr, disp, 5, "Done", "Night"));
    ui::Row night[3] = {{"Night starts", "23:00", ui::ROW_STEPPER}, {"Night ends", "07:00", ui::ROW_STEPPER}, {"Night dim", "10%", ui::ROW_STEPPER}};
    SHOT("25_settings_night", ui::drawMenu("Night", nullptr, night, 3, "Done", "System"));
    // the fullest System page: an update, a manual time zone, a paired account
    ui::Row sys[5] = {{"Update", "0.3.3 available", ui::ROW_ACTION},
                      {"Time zone", "Manual", ui::ROW_CYCLE},
                      {"UTC offset", "+5:30", ui::ROW_STEPPER},
                      {"Wi-Fi", "Guest_Network_With_A_Long_Name", ui::ROW_ACTION},
                      {"Disconnect", "", ui::ROW_ACTION}};
    SHOT("19_settings_system", ui::drawMenu("System", "v0.3.3", sys, 5, "Done", "Display"));
    // the longest value a settings row carries fits beside its label, upright too
    CHECK(gx::textW(F_MED, "Time zone") + 12 + gx::textW(F_MED, "Auto (+14:00)") <= gx::W - 20 - 32);
    ui::Row sysAuto[3] = {{"Time zone", "Auto (+4:00)", ui::ROW_CYCLE},
                          {"Wi-Fi", "Home 5G", ui::ROW_ACTION},
                          {"Disconnect", "", ui::ROW_ACTION}};
    SHOT("26_settings_system_auto", ui::drawMenu("System", "v0.3.3", sysAuto, 3, "Done", "Display"));
    SHOT("27_update", ui::drawUpdate("0.3.3", "app.sugarsense.io/install"));
    // a newer firmware exists: the quiet dot beside the three
    series(s, 3, 2, 104, 26, false);
    ui::updateHint = true;
    SHOT("28_dark_update_dot", ui::drawHome(v, ui::LOOK_DARK));
    SHOT("29_color_update_dot", ui::drawHome(v, ui::LOOK_COLOR));
    ui::updateHint = false;
    SHOT("20_confirm", ui::drawConfirm("Disconnect", "This display will stop", "showing your glucose", "until you pair it again.", "Cancel", "Disconnect"));
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::string(argv[1]) == "logic") return runLogic();
  if (argc >= 6 && std::string(argv[1]) == "tail") {
    const bool mmol = atoi(argv[2]) != 0;
    const int64_t now = 1800000000000LL;
    ss::Point p[2] = {{now - (int64_t)atoi(argv[3]) * 1000, atof(argv[4])}, {now, atof(argv[5])}};
    double d = 0;
    if (!ss::deltaFromPoints(p, 2, now, now, &d)) {
      printf("null\n");
      return 0;
    }
    printf("%s\n", delta(d, mmol).c_str());
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "render") return runRender(argv[2]);
  fprintf(stderr, "usage: host logic | tail <mmol> <gapSec> <prev> <new> | render <dir>\n");
  return 2;
}
