// Wi-Fi, the wall clock and the four Sugar Sense calls the display makes.
//
// The display is a read-only client. On the network it talks to
// app.sugarsense.io, over TLS checked against the root certificates in
// certs.h, and to the public time servers (pool.ntp.org, time.google.com)
// for the clock; nothing else:
//   POST /displayPairStart {}                     -> a 6-character code, a poll
//                                                    secret and the address of
//                                                    the pairing page
//   POST /displayPairPoll  {pollSecret, deviceId} -> pending, then ONE session
//                                                    token (displayToken)
//   POST /getDisplaySnapshot {hours, people?}     -> the latest reading, the
//        Authorization: Bearer <displayToken>        account's limits and unit,
//                                                    the trend points and the
//                                                    clock offset; `followed`
//                                                    (everyone the account
//                                                    follows) and `people`
//                                                    (the followed ones asked
//                                                    for, each with their own
//                                                    reading) since 0.4.0
//   POST /displayUnpair {}                        -> ends the session
//        Authorization: Bearer <displayToken>        ("Disconnect")
// A device with no keyboard shows a code; the person confirms it on a page
// where they are signed in. No sensor password ever reaches the device, and
// the token it holds can only read this one snapshot.
#ifndef SS_NET_H
#define SS_NET_H

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include "certs.h"
#include "qrcode.h"
#include "ui.h"

#define SS_API "https://app.sugarsense.io/api/v1"
// The page where the person confirms the code. The server names it on every
// pairing start (so a display flashed once survives a page move); these are
// the values used when it does not. The QR code carries the address with the
// code inside, the screen prints the address the way a person would type it.
#define SS_PAIR_URL "https://app.sugarsense.io/display"
#define SS_PAIR_URL_LABEL "app.sugarsense.io/display"
#define SS_MODEL "JC3248W535"
// Where a phone gets the app (the "one more step" screen draws it as a QR
// code). The server sends its own with the no-source answer; this is the
// value used when it does not.
#define SS_APP_URL "https://sugarsense.io/download"
// Where a newer firmware is installed from (the "Update available" page prints
// it). The server sends its own with every answer; this is the fallback.
#define SS_INSTALL_URL "https://app.sugarsense.io/install"

static WiFiClientSecure net_tls;
static HTTPClient net_http;   // one long-lived client, so a connection can be kept between pulls
static char net_device_id[24] = "";

static void net_begin() {
  // "disp-" + the chip's factory MAC: how the server tells displays from watches
  uint64_t mac = ESP.getEfuseMac();
  snprintf(net_device_id, sizeof(net_device_id), "disp-%02x%02x%02x%02x%02x%02x", (unsigned)(mac & 0xFF), (unsigned)((mac >> 8) & 0xFF),
           (unsigned)((mac >> 16) & 0xFF), (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  net_tls.setCACert(SS_ROOT_CAS);
  net_http.setReuse(true);
  // UTC only: the display adds its own offset (Settings), it keeps no zone database.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

static bool net_wifi_up() { return WiFi.status() == WL_CONNECTED; }

// ---- the wall clock ------------------------------------------------------------
static bool clock_synced() { return time(nullptr) > 1700000000; }

static int64_t clock_now_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Every answer from our server carries the time in its Date header. When the
// clock is unset or has drifted by more than a minute, take it from there, so
// the display keeps time on a network that blocks NTP.
static void clock_adopt(const String &dateHeader) {
  int64_t server = 0;
  if (!ss::parseHttpDate(dateHeader.c_str(), &server)) return;
  int64_t drift = server - (int64_t)time(nullptr);
  if (drift > -60 && drift < 60) return;
  struct timeval tv = {(time_t)server, 0};
  settimeofday(&tv, nullptr);
}

// ---- one JSON POST ---------------------------------------------------------------
// Returns the HTTP status (the body parsed into `doc` on 200), or a negative
// number when the request never completed.
static int api_post(const char *path, const String &body, const char *bearer, JsonDocument &doc) {
  for (int attempt = 0; attempt < 2; attempt++) {
    net_http.setConnectTimeout(8000);
    net_http.setTimeout(10000);
    if (!net_http.begin(net_tls, String(SS_API) + path)) return -1;
    static const char *wanted[] = {"Date"};
    net_http.collectHeaders(wanted, 1);
    net_http.setUserAgent("SugarSenseDisplay/" FW_VERSION);
    net_http.addHeader("Content-Type", "application/json");
    net_http.addHeader("Accept", "application/json");
    if (bearer && bearer[0]) net_http.addHeader("Authorization", String("Bearer ") + bearer);
    int status = net_http.POST(body);
    if (status > 0) {
      if (net_http.hasHeader("Date")) clock_adopt(net_http.header("Date"));
      String answer = net_http.getString();
      net_http.end();
      if (status == 200 && deserializeJson(doc, answer) != DeserializationError::Ok) return -100;
      return status;
    }
    // the kept connection was closed by the far end: once more on a fresh one
    net_http.end();
    net_tls.stop();
  }
  return -1;
}

// ---- pairing ---------------------------------------------------------------------
struct PairStart {
  char code[8];
  char secret[64];
  char url[112];     // what the QR code carries: the pairing page with the code inside
  char label[48];    // the same page as a person would type it
  int expiresS;
  int pollS;
};

static int api_pair_start(PairStart &out) {
  JsonDocument doc;
  int status = api_post("/displayPairStart", "{}", nullptr, doc);
  if (status != 200) return status;
  JsonObject r = doc["response"];
  const char *code = r["code"] | "";
  const char *secret = r["pollSecret"] | "";
  if (!code[0] || !secret[0] || strlen(code) >= sizeof(out.code) || strlen(secret) >= sizeof(out.secret)) return -101;
  snprintf(out.code, sizeof(out.code), "%s", code);
  snprintf(out.secret, sizeof(out.secret), "%s", secret);
  out.expiresS = r["expiresInSeconds"] | 600;
  out.pollS = r["pollIntervalSeconds"] | 5;
  if (out.pollS < 2) out.pollS = 2;
  // the server's own address for the page, when it sent a usable one
  const char *url = r["pairUrl"] | "";
  const char *label = r["pairLabel"] | "";
  if (strncmp(url, "https://", 8) == 0 && strlen(url) < sizeof(out.url)) snprintf(out.url, sizeof(out.url), "%s", url);
  else snprintf(out.url, sizeof(out.url), SS_PAIR_URL "?c=%s", out.code);
  if (label[0] && strlen(label) < sizeof(out.label)) ui::asciiLabel(label, out.label, sizeof(out.label));
  else snprintf(out.label, sizeof(out.label), "%s", SS_PAIR_URL_LABEL);
  return 200;
}

// 200 + *pending = still waiting for the person; 200 + a token = paired; 404 =
// the code expired or was used, start over. With the token comes the clock
// offset of the phone that confirmed the code, when the server has it.
static int api_pair_poll(const char *secret, bool *pending, char *token, size_t tokenLen, bool *hasOffset, int *offsetMin) {
  JsonDocument body;
  body["pollSecret"] = secret;
  body["deviceId"] = net_device_id;
  body["model"] = SS_MODEL;
  body["appVersion"] = FW_VERSION;
  String payload;
  serializeJson(body, payload);
  JsonDocument doc;
  int status = api_post("/displayPairPoll", payload, nullptr, doc);
  if (status != 200) return status;
  JsonObject r = doc["response"];
  *pending = r["pending"] | true;
  if (*pending) return 200;
  const char *t = r["displayToken"] | "";
  if (strlen(t) != 43 || strlen(t) >= tokenLen) return -102;
  snprintf(token, tokenLen, "%s", t);
  *hasOffset = r["utcOffsetMinutes"].is<int>();
  *offsetMin = r["utcOffsetMinutes"] | 0;
  return 200;
}

// "Disconnect": end the session on the server too. Best effort: the display
// forgets its token whatever this returns.
static int api_unpair(const char *token) {
  JsonDocument doc;
  return api_post("/displayUnpair", "{}", token, doc);
}

// ---- the snapshot ------------------------------------------------------------------
// One reading as the server sends it (the account's own at the top level, a
// followed person's inside `people`): the value, its moment, the arrow, the
// unit, the four limits and the points of the window.
static void read_reading(JsonObject r, int hours, bool *hasValue, double *mgdl, int64_t *dateMs, int *arrow, bool *isMmol, ss::Band *band, ss::Point *pts, int *npts, int *outHours) {
  *hasValue = !r["value"].isNull() && (r["date"] | (int64_t)0) > 0;
  *mgdl = r["value"] | 0.0;
  *dateMs = r["date"] | (int64_t)0;
  *arrow = r["arrowDirection"] | 3;
  *isMmol = r["isMmol"] | false;
  *outHours = r["hours"] | hours;
  band->veryLow = r["veryLowLimit"] | ss::DEFAULT_BAND.veryLow;
  band->low = r["lowLimit"] | ss::DEFAULT_BAND.low;
  band->high = r["highLimit"] | ss::DEFAULT_BAND.high;
  band->veryHigh = r["veryHighLimit"] | ss::DEFAULT_BAND.veryHigh;
  *npts = 0;
  JsonArray arr = r["points"].as<JsonArray>();
  int skip = (int)arr.size() - SS_MAX_POINTS;   // keep the newest when there are too many
  for (JsonArray p : arr) {
    if (skip-- > 0) continue;
    if (p.size() < 2 || *npts >= SS_MAX_POINTS) continue;
    pts[*npts].dateMs = p[0] | (int64_t)0;
    pts[*npts].mgdl = p[1] | 0.0;
    (*npts)++;
  }
}

// 200 fills `out` (its `state` says what the account can show); 401 = the
// session is gone, pair again. `people` names the followed people the screen
// shows (their ids, comma-separated, at most four); `peoplePoints` asks for
// the whole window of the one person shown alone, else each person comes
// with the two newest readings only (enough for the change).
static int api_snapshot(const char *token, int hours, const char *people, bool peoplePoints, ui::Snapshot &out) {
  JsonDocument body;
  body["hours"] = hours;
  body["model"] = SS_MODEL;
  body["appVersion"] = FW_VERSION;
  if (people && people[0]) {
    JsonArray ids = body["people"].to<JsonArray>();
    const char *p = people;
    while (*p) {
      const char *e = strchr(p, ',');
      const size_t len = e ? (size_t)(e - p) : strlen(p);
      char id[48];
      if (len > 0 && len < sizeof(id)) {
        memcpy(id, p, len);
        id[len] = '\0';
        ids.add(id);
      }
      if (!e) break;
      p = e + 1;
    }
    if (peoplePoints) body["peoplePoints"] = true;
  }
  String payload;
  serializeJson(body, payload);
  JsonDocument doc;
  int status = api_post("/getDisplaySnapshot", payload, token, doc);
  if (status != 200) return status;
  JsonObject r = doc["response"];
  if (r.isNull() || r["state"].isNull()) return -103;
  static ui::Snapshot s;   // the net task's own (too large for its stack)
  s = ui::Snapshot();
  s.state = r["state"] | (int)ss::STATE_NONE;
  // on every answer, whatever the state: the clock offset and the cadence
  s.hasOffset = r["utcOffsetMinutes"].is<int>();
  s.offsetMin = constrain((int)(r["utcOffsetMinutes"] | 0), -720, 840);
  s.pullS = constrain((int)(r["pullSeconds"] | 60), 30, 900);
  // the firmware version the install page serves, and that page
  const char *latest = r["latestVersion"] | "";
  if (latest[0] >= '0' && latest[0] <= '9' && strlen(latest) < sizeof(s.latest)) snprintf(s.latest, sizeof(s.latest), "%s", latest);
  const char *install = r["installUrl"] | "";
  snprintf(s.installUrl, sizeof(s.installUrl), "%s", strncmp(install, "https://", 8) == 0 && strlen(install) < sizeof(s.installUrl) ? install : SS_INSTALL_URL);
  const char *app = r["appUrl"] | "";
  snprintf(s.appUrl, sizeof(s.appUrl), "%s", strncmp(app, "https://", 8) == 0 && strlen(app) < sizeof(s.appUrl) ? app : SS_APP_URL);
  if (s.state == ss::STATE_OK) {
    read_reading(r, hours, &s.hasValue, &s.mgdl, &s.dateMs, &s.arrow, &s.isMmol, &s.band, s.pts, &s.npts, &s.hours);
  }
  // everyone this account follows (the People page), then the ones asked for
  for (JsonObject f : r["followed"].as<JsonArray>()) {
    if (s.nfollowed >= SS_MAX_FOLLOWED) break;
    const char *id = f["id"] | "";
    const char *label = f["label"] | "";
    if (!id[0] || strlen(id) >= sizeof(s.followed[0].id)) continue;
    snprintf(s.followed[s.nfollowed].id, sizeof(s.followed[0].id), "%s", id);
    ui::asciiLabel(label, s.followed[s.nfollowed].label, sizeof(s.followed[0].label));
    s.nfollowed++;
  }
  for (JsonObject p : r["people"].as<JsonArray>()) {
    if (s.npeople >= SS_MAX_TILES) break;
    const char *id = p["id"] | "";
    const char *label = p["label"] | "";
    if (!id[0] || strlen(id) >= sizeof(s.people[0].id)) continue;
    ui::Person &q = s.people[s.npeople];
    snprintf(q.id, sizeof(q.id), "%s", id);
    ui::asciiLabel(label, q.label, sizeof(q.label));
    q.state = p["state"] | (int)ss::STATE_FOLLOWER;
    q.paused = !p["sharingPaused"].isNull();
    if (q.state == ss::STATE_OK) {
      read_reading(p, hours, &q.hasValue, &q.mgdl, &q.dateMs, &q.arrow, &q.isMmol, &q.band, q.pts, &q.npts, &q.hours);
    }
    s.npeople++;
  }
  out = s;
  return 200;
}

// ---- the QR code of the pairing page ----------------------------------------------
static uint8_t qr_mods[45 * 45];
static int qr_n = 0;

static void qr_capture(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  if (n <= 0 || n > 45) return;
  qr_n = n;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) qr_mods[y * n + x] = esp_qrcode_get_module(q, x, y) ? 1 : 0;
}

static void qr_make(const char *text) {
  esp_qrcode_config_t cfg;
  cfg.display_func = qr_capture;
  cfg.max_qrcode_version = 7;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  qr_n = 0;
  esp_qrcode_generate(&cfg, text);
}

#endif
