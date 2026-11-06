// Sugar Sense display: glucose at a glance on a small touchscreen
// (Guition JC3248W535: ESP32-S3, 3.5 inch, 320x480).
//
// It pairs with a Sugar Sense account by a 6-character code, then pulls that
// account's glucose once a minute and shows it in the Sugar Sense app's own
// look: a dark screen with the number in its zone color and the app's graph,
// a black bedside clock at night, or the whole screen in the zone color.
// An account that follows people in the Care Circle can show them too, up
// to four at once on a split screen (Settings > People).
//
// NOT A MEDICAL DEVICE. It is a second screen for readings you already have
// in the app. It never alerts, and past 15 minutes without a reading it says
// "No Data" instead of showing an old number. Treatment decisions belong on
// your phone and your sensor's own app.
//
// Files: ss_logic.h (the display rules), gx.h (the renderer), ui.h (every
// screen), hw.h (the board), store.h (settings), net.h (Wi-Fi + the API).

#define FW_VERSION "0.4.2"

#include "ui.h"
#include "hw.h"
#include "store.h"
#include "net.h"

// TLS with certificate checks needs more stack than the default 8 KB loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static ui::Snapshot snap;
static uint32_t snap_seq = 0;        // bumps on every snapshot, for the redraw check
static bool api_ok = true;
static bool need_pair = false;
static uint32_t wake_until_ms = 0;   // a tap in the night hours brightens the screen until then
static bool dirty = true;
static uint32_t last_draw_ms = 0, last_flush_ms = 0, last_pull_took_ms = 0;   // how long the last frame and pull took

static void tick() {
  if (prefs.sound) hw_note(1500, 14, 22);
}

static void chime() {
  if (!prefs.sound) return;
  hw_note(660, 80, 28);
  hw_note(880, 110, 28);
}

// The one QR buffer carries the pairing page while pairing, and the
// get-the-app page while the paired account has no glucose source.
static char qr_app_url[72] = "";

static ui::View make_view() {
  ui::View v;
  v.snap = &snap;
  if (snap.state == ss::STATE_NO_SOURCE) {
    const char *url = snap.appUrl[0] ? snap.appUrl : SS_APP_URL;
    if (strcmp(qr_app_url, url) != 0) {
      qr_make(url);
      snprintf(qr_app_url, sizeof(qr_app_url), "%s", url);
    }
    v.qr = qr_mods;
    v.qrN = qr_n;
    v.qrLabel = qr_app_url + 8;   // without "https://", as a person would type it
  }
  v.nowMs = clock_now_ms();
  v.tzMin = prefs.tzMin;
  v.showGraph = prefs.graphHours != 0;
  v.graphHours = (snap.state == ss::STATE_OK && snap.hours) ? snap.hours : (prefs.graphHours ? prefs.graphHours : 3);   // the window the readings cover
  v.st.wifi = net_wifi_up();
  v.st.apiOk = api_ok;
  v.st.timeSynced = clock_synced();
  return v;
}

// ---- whose glucose the screen shows --------------------------------------------
// The account's own reading ("me") and the followed people the person chose
// in Settings > People (prefs.people). Nothing chosen = the own reading when
// the account has a source, else the first followed person. A chosen person
// who is no longer followed drops out by himself.
static const char *ME = "me";
static ui::Snapshot person_snaps[SS_MAX_TILES];   // a followed person's reading as a Snapshot, for the drawers
static ui::TileSource tiles[SS_MAX_TILES];
static int ntiles = 0;
static char ask_people[160] = "";   // the followed ids the next pull asks for
static bool ask_points = false;

static bool own_has_source() { return snap.state == ss::STATE_OK; }

static int followed_index(const char *id) {
  for (int i = 0; i < snap.nfollowed; i++)
    if (strcmp(snap.followed[i].id, id) == 0) return i;
  return -1;
}

static const ui::Person *person_for(const char *id) {
  for (int i = 0; i < snap.npeople; i++)
    if (strcmp(snap.people[i].id, id) == 0) return &snap.people[i];
  return nullptr;
}

// The ids the screen shows, in order: "me" first when chosen, then the
// followed people in the order they are followed. Returns the count; `ids`
// point into prefs / snap.
static int chosen_ids(const char **ids) {
  int n = 0;
  const bool chosen = prefs.people[0] != '\0';
  if (own_has_source() && (!chosen || ss::selectionHas(prefs.people, ME))) ids[n++] = ME;
  for (int i = 0; i < snap.nfollowed && n < SS_MAX_TILES; i++) {
    if (chosen ? ss::selectionHas(prefs.people, snap.followed[i].id) : (n == 0 && i == 0)) ids[n++] = snap.followed[i].id;
  }
  if (n == 0 && own_has_source()) ids[n++] = ME;
  if (n == 0 && snap.nfollowed > 0) ids[n++] = snap.followed[0].id;
  return n;
}

// Rebuild the tiles from the newest snapshot, and remember what the next pull
// must ask for (the followed ids among the chosen, the whole window when one
// followed person stands alone).
static void rebuild_tiles() {
  const char *ids[SS_MAX_TILES];
  const int n = chosen_ids(ids);
  ntiles = 0;
  ask_people[0] = '\0';
  int followedCount = 0;
  for (int i = 0; i < n; i++) {
    ui::TileSource &t = tiles[ntiles];
    if (strcmp(ids[i], ME) == 0) {
      t = {&snap, "Me", true, false, false};
      ntiles++;
      continue;
    }
    ss::selectionToggle(ask_people, sizeof(ask_people), ids[i]);   // bounded: never past the room, never a fifth
    followedCount++;
    const int fi = followed_index(ids[i]);
    const ui::Person *p = person_for(ids[i]);
    ui::Snapshot &ps = person_snaps[ntiles];
    ps = ui::Snapshot();
    if (p) {
      ps.state = p->state == ss::STATE_OK ? ss::STATE_OK : ss::STATE_FOLLOWER;
      ps.hasValue = p->hasValue;
      ps.mgdl = p->mgdl;
      ps.dateMs = p->dateMs;
      ps.arrow = p->arrow;
      ps.isMmol = p->isMmol;
      ps.band = p->band;
      memcpy(ps.pts, p->pts, sizeof(ps.pts));
      ps.npts = p->npts;
      ps.hours = p->hours;
    }
    // not answered yet (ps stays at STATE_NONE): the tile says "Connecting" until the re-ask lands
    t = {&ps, fi >= 0 ? snap.followed[fi].label : (p ? p->label : "Person"), false, p ? p->state != ss::STATE_OK : false, p ? p->paused : false};
    ntiles++;
  }
  ask_points = (n == 1 && followedCount == 1);
}

// The server names the firmware the install page serves; a newer one than
// this build is an update the person can install.
static bool update_available() { return snap.latest[0] && ss::compareVersions(snap.latest, FW_VERSION) > 0; }

static bool night_hours() {
  if (!clock_synced()) return false;
  int h, m;
  ss::localHourMinute(clock_now_ms(), prefs.tzMin, &h, &m);
  return ss::isNightHour(h, prefs.nightStart, prefs.nightEnd);
}

// Which scene is up: the chosen one, or on Auto (the default) the Color scene
// by day and the Night scene in the night hours (ss::lookForScene).
static int current_look() { return ss::lookForScene(prefs.scene, night_hours()); }
static bool awake() { return (int32_t)(wake_until_ms - millis()) > 0; }
static void apply_backlight() { hw_backlight(night_hours() && !awake() ? prefs.nightBright : prefs.bright); }

static void print_info() {
  int age = ss::elapsedMinutes(clock_now_ms(), snap.dateMs);
  Serial.printf("INFO v%s up=%lus id=%s rot=%d wifi=%d rssi=%d paired=%d state=%d value=%.1f ageMin=%d points=%d api=%d synced=%d look=%d light=%d taps=%u reads=%u/%u/%u draw=%ums flush=%ums pull=%ums stages=%u/%u/%u heap=%u\n",
                FW_VERSION, (unsigned long)(millis() / 1000), net_device_id, prefs.rot, net_wifi_up() ? 1 : 0, net_wifi_up() ? (int)WiFi.RSSI() : 0,
                prefs.token[0] ? 1 : 0, snap.state, snap.hasValue ? snap.mgdl : 0.0, age, snap.npts, api_ok ? 1 : 0, clock_synced() ? 1 : 0,
                current_look(), hw_backlight_now, (unsigned)touch_taps, (unsigned)touch_finger, (unsigned)touch_none, (unsigned)touch_bad,
                (unsigned)last_draw_ms, (unsigned)last_flush_ms, (unsigned)last_pull_took_ms,
                (unsigned)(ui::stageUs[0] / 1000), (unsigned)(ui::stageUs[1] / 1000), (unsigned)(ui::stageUs[2] / 1000), (unsigned)ESP.getFreeHeap());
}

static void service_info() {
  if (!hw_info_requested) return;
  hw_info_requested = false;
  print_info();
}

static void wait_tap() {
  while (!hw_tap()) {
    service_info();
    delay(5);
  }
}

// ---- pulls in the background -------------------------------------------------------
// The app's loop never waits for the server. A task of its own makes each
// pull (a TLS exchange takes 0.4 to 2 seconds) and hands the answer over, so
// a tap is answered at once whatever the network is doing.
static SemaphoreHandle_t net_lock = nullptr;
static TaskHandle_t net_task_handle = nullptr;
static volatile int net_paused = 1;      // > 0 while a screen uses the network itself (Wi-Fi, pairing)
static volatile bool net_busy = false;   // a pull is on the wire
static volatile bool net_kick = false;   // pull now, not at the next minute
static struct {
  char token[48];
  int hours;
  char people[160];   // the followed ids the screen shows
  bool points;        // the whole window for the one followed person shown alone
} net_ask;
static struct {
  ui::Snapshot snap;
  int rc;
  uint32_t took;
} net_answer;
static volatile uint32_t net_answer_seq = 0;
static uint32_t net_seen_seq = 0;

static void net_task(void *) {
  static ui::Snapshot fresh;
  uint32_t last_done = 0, gap = 0;   // gap 0 = nothing pulled yet
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    if (net_paused > 0 || !net_wifi_up()) continue;
    if (!net_kick && gap && millis() - last_done < gap) continue;
    char token[48];
    int hours;
    static char people[160];
    bool points;
    xSemaphoreTake(net_lock, portMAX_DELAY);
    memcpy(token, net_ask.token, sizeof(token));
    hours = net_ask.hours;
    memcpy(people, net_ask.people, sizeof(people));
    points = net_ask.points;
    xSemaphoreGive(net_lock);
    if (!token[0]) continue;
    net_kick = false;
    net_busy = true;
    const uint32_t started = millis();
    const int rc = api_snapshot(token, hours, people, points, fresh);
    const uint32_t took = millis() - started;
    trace(TR_PULL, (int)took, rc);
    xSemaphoreTake(net_lock, portMAX_DELAY);
    if (rc == 200) net_answer.snap = fresh;
    net_answer.rc = rc;
    net_answer.took = took;
    net_answer_seq = net_answer_seq + 1;
    xSemaphoreGive(net_lock);
    last_done = millis();
    gap = rc == 200 ? (uint32_t)fresh.pullS * 1000 : (rc == 429 ? 300000 : 20000);   // the server names the cadence
    net_busy = false;
    Serial.printf("pull: http=%d took=%ums\n", rc, (unsigned)took);
  }
}

// Tell the task what to ask for (the session, the graph window); `now` = at once.
static void net_ask_for(bool now) {
  xSemaphoreTake(net_lock, portMAX_DELAY);
  snprintf(net_ask.token, sizeof(net_ask.token), "%s", prefs.token);
  net_ask.hours = prefs.graphHours ? prefs.graphHours : 3;   // without a graph the short window still carries the change
  snprintf(net_ask.people, sizeof(net_ask.people), "%s", ask_people);
  net_ask.points = ask_points;
  xSemaphoreGive(net_lock);
  if (!now) return;
  net_kick = true;
  if (net_task_handle) xTaskNotifyGive(net_task_handle);
}

// Hold the task off while a screen uses the network itself, and wait out a
// pull that is on the wire.
static void net_pause() {
  net_paused = net_paused + 1;
  for (uint32_t t0 = millis(); net_busy && millis() - t0 < 20000;) delay(20);
}

// Let it pull again, from now, with the current session and window. An answer
// that came in before or during the pause is not for this session: dropped.
static void net_resume() {
  net_ask_for(true);
  net_seen_seq = net_answer_seq;
  if (net_paused > 0) net_paused = net_paused - 1;
}

// The clock follows the phone that paired the display: the server sends the
// offset on every answer (and a clock change with it). "Manual" in Settings
// keeps the offset set by hand.
static void adopt_offset(bool has, int offsetMin) {
  if (!has || !prefs.tzAuto || offsetMin == prefs.tzMin) return;
  prefs.tzMin = (int16_t)constrain(offsetMin, -720, 840);
  store_save();
  dirty = true;
}

// Take over an answer the task has ready.
static void net_collect() {
  if (net_answer_seq == net_seen_seq) return;
  xSemaphoreTake(net_lock, portMAX_DELAY);
  net_seen_seq = net_answer_seq;
  const int rc = net_answer.rc;
  last_pull_took_ms = net_answer.took;
  if (rc == 200) snap = net_answer.snap;
  xSemaphoreGive(net_lock);
  trace(TR_ANSWER, rc, 0);
  if (rc == 200) {
    snap_seq++;
    api_ok = true;
    adopt_offset(snap.hasOffset, snap.offsetMin);
    // the people the screen shows, from this answer; the next pull always asks
    // for exactly these (a person no longer followed leaves the ask too), and
    // when the answer did not carry them yet (the first pull, a change in
    // Settings) it asks again now
    const bool pointsBefore = ask_points;
    rebuild_tiles();
    char carried[160] = "";
    for (int i = 0; i < snap.npeople; i++) ss::selectionToggle(carried, sizeof(carried), snap.people[i].id);
    net_ask_for(strcmp(carried, ask_people) != 0 || pointsBefore != ask_points);
  } else if (rc == 401) {   // the session was ended on the account: pair again
    prefs.token[0] = '\0';
    store_save();
    snap = ui::Snapshot();
    snap_seq++;
    need_pair = true;
  } else {
    api_ok = false;
  }
}

// ---- the keyboard ---------------------------------------------------------------
// Modal: returns true on OK with the text in `out`, false on Back.
static bool kb_input(const char *title, char *out, int outlen, bool mask) {
  int len = (int)strlen(out), layer = 0;
  hw_touch_flush();
  for (;;) {
    char shown[80];
    int n = len < (int)sizeof(shown) - 1 ? len : (int)sizeof(shown) - 1;
    for (int i = 0; i < n; i++) shown[i] = (mask && i < n - 1) ? '*' : out[len - n + i];   // the last character stays readable
    shown[n] = '\0';
    ui::drawKeyboard(title, shown, layer);
    hw_present();
    ui::Key keys[KB_MAX_KEYS];
    int count = ui::kbBuild(layer, keys);
    const ui::Key *key = nullptr;
    while (!key) {
      wait_tap();
      key = ui::kbHit(tap_x, tap_y, keys, count);
    }
    tick();
    switch (key->kind) {
      case ui::KEY_OK: return true;
      case ui::KEY_CANCEL: return false;
      case ui::KEY_SHIFT: layer = layer == 0 ? 1 : (layer == 1 ? 0 : (layer == 2 ? 3 : 2)); break;
      case ui::KEY_LAYER: layer = layer < 2 ? 2 : 0; break;
      case ui::KEY_BACKSPACE:
        if (len > 0) out[--len] = '\0';
        break;
      default: {
        char c = key->kind == ui::KEY_SPACE ? ' ' : key->ch;
        if (len < outlen - 1) {
          out[len++] = c;
          out[len] = '\0';
        }
        if (layer == 1) layer = 0;   // one capital, then back to lower case
      }
    }
  }
}

// ---- Wi-Fi ----------------------------------------------------------------------
static bool wifi_join(const char *ssid, const char *pass) {
  char label[33];
  ui::asciiLabel(ssid, label, sizeof(label));
  ui::drawPageNote("Wi-Fi", "Connecting to", label, ui::C_TEXT);
  hw_present();
  WiFi.disconnect(true);
  delay(150);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
  for (uint32_t t0 = millis(); millis() - t0 < 20000;) {
    if (net_wifi_up()) return true;
    delay(200);
  }
  return false;
}

// The network picker. On the first run the left button turns the screen (the
// stand decides the orientation before anything is typed); from Settings it
// goes back. Returns true when the board is on a network.
static bool flow_wifi_inner(bool firstRun) {
  static char names[8][33];    // the real network names, used to connect
  static char labels[8][33];   // what the list draws
  static int rssi[8];
  static bool open[8];
  const char *shown[8];
  for (;;) {
    ui::drawPageNote("Wi-Fi", "Looking for networks" CH_ELL, nullptr, 0);
    hw_present();
    WiFi.mode(WIFI_STA);
    if (!net_wifi_up()) {   // a scan fails while the radio is still trying to join
      WiFi.disconnect();
      delay(200);
    }
    WiFi.scanDelete();
    int found = WiFi.scanNetworks();
    int count = 0;
    for (int i = 0; i < found && count < 8; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0 || s.length() > 32) continue;
      bool seen = false;
      for (int k = 0; k < count; k++) seen = seen || s == names[k];
      if (seen) continue;
      snprintf(names[count], sizeof(names[count]), "%s", s.c_str());
      rssi[count] = WiFi.RSSI(i);
      open[count] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      ui::asciiLabel(names[count], labels[count], sizeof(labels[count]));
      shown[count] = labels[count];
      count++;
    }
    for (bool rescan = false; !rescan;) {
      ui::drawWifiList(shown, rssi, count, "Choose your network", firstRun ? "Rotate" : "Back");
      hw_present();
      wait_tap();
      tick();
      ui::MenuLayout L = ui::menuLayout();
      if (L.right().has(tap_x, tap_y)) {
        rescan = true;
        continue;
      }
      if (L.left().has(tap_x, tap_y)) {
        if (firstRun) {
          prefs.rot = (prefs.rot + 1) & 3;
          gx::setRotation(prefs.rot);
          store_save();
          continue;
        }
        if (!net_wifi_up() && prefs.ssid[0]) WiFi.begin(prefs.ssid, prefs.wpass);
        return net_wifi_up();
      }
      int pick = -1;
      for (int i = 0; i < count && i < L.maxRows; i++)
        if (L.row(i).has(tap_x, tap_y)) pick = i;
      if (pick < 0) continue;
      char pass[65] = "";
      bool joined = false;
      if (open[pick]) {
        joined = wifi_join(names[pick], "");
      } else {
        // a network this display already knows: its stored password first
        if (strcmp(names[pick], prefs.ssid) == 0 && prefs.wpass[0]) {
          snprintf(pass, sizeof(pass), "%s", prefs.wpass);
          joined = wifi_join(names[pick], pass);
        }
        if (!joined) {
          pass[0] = '\0';
          char title[64], cut[64];
          snprintf(title, sizeof(title), "Password for %.32s", labels[pick]);
          ui::fitText(F_SMALL, title, gx::W - 32, cut, sizeof(cut));
          if (!kb_input(cut, pass, sizeof(pass), true)) continue;
          joined = wifi_join(names[pick], pass);
        }
      }
      if (joined) {
        snprintf(prefs.ssid, sizeof(prefs.ssid), "%s", names[pick]);
        snprintf(prefs.wpass, sizeof(prefs.wpass), "%s", pass);
        store_save();
        chime();
        Serial.println("wifi: joined");
        return true;
      }
      ui::drawPageNote("Wi-Fi", "Could not connect", "Tap to choose again", ui::C_DIM);
      hw_present();
      wait_tap();
    }
  }
}

static bool flow_wifi(bool firstRun) {
  net_pause();
  const bool up = flow_wifi_inner(firstRun);
  net_resume();
  return up;
}

// ---- settings ---------------------------------------------------------------------
static bool confirm_disconnect() {
  ui::drawConfirm("Disconnect", "This display will stop", "showing your glucose", "until you pair it again.", "Cancel", "Disconnect");
  hw_present();
  hw_touch_flush();
  for (;;) {
    wait_tap();
    tick();
    ui::MenuLayout L = ui::menuLayout();
    if (L.left().has(tap_x, tap_y)) return false;
    if (L.right().has(tap_x, tap_y)) return true;
  }
}

// Three short pages. Returns true when the person disconnected the account.
static bool flow_settings() {
  static const char *SCENES[4] = {"Auto", "Dark", "Night", "Color"};
  static const char *PAGES[4] = {"Display", "Night", "System", "People"};
  int page = 0;
  hw_backlight(prefs.bright);
  hw_touch_flush();
  enum { R_BRIGHT, R_SCENE, R_GRAPH, R_ROTATE, R_NSTART, R_NEND, R_NDIM, R_UPDATE, R_ZONE, R_OFFSET, R_SOUND, R_WIFI, R_DISCONNECT, R_PERSON };
  for (;;) {
    // the People page exists while the account follows someone
    const int npages = snap.nfollowed > 0 ? 4 : 3;
    if (page >= npages) page = 0;
    ui::Row rows[6];
    int ids[6];               // which setting each row on the screen is
    const char *who[6] = {};  // the People page: the id behind each row
    int n = 0;
    char offset[12];
    const int tz = prefs.tzMin < 0 ? -prefs.tzMin : prefs.tzMin;
    snprintf(offset, sizeof(offset), "%s%d:%02d", prefs.tzMin < 0 ? CH_MINUS : "+", tz / 60, tz % 60);
    if (page == 0) {
      ids[n] = R_BRIGHT;
      rows[n] = {"Brightness", "", ui::ROW_STEPPER};
      snprintf(rows[n++].value, sizeof(rows[0].value), "%d%%", prefs.bright);
      ids[n] = R_SCENE;
      rows[n] = {"Scene", "", ui::ROW_CYCLE};
      snprintf(rows[n++].value, sizeof(rows[0].value), "%s", SCENES[prefs.scene % 4]);
      ids[n] = R_GRAPH;
      rows[n] = {"Graph", "", ui::ROW_CYCLE};
      if (prefs.graphHours) snprintf(rows[n].value, sizeof(rows[0].value), "%d hours", prefs.graphHours);
      else snprintf(rows[n].value, sizeof(rows[0].value), "Off");
      n++;
      ids[n] = R_ROTATE;
      rows[n++] = {"Rotate screen", "", ui::ROW_ACTION};
      ids[n] = R_SOUND;
      rows[n] = {"Sound", "", ui::ROW_CYCLE};
      snprintf(rows[n++].value, sizeof(rows[0].value), "%s", prefs.sound ? "On" : "Off");
    } else if (page == 1) {
      ids[n] = R_NSTART;
      rows[n] = {"Night starts", "", ui::ROW_STEPPER};
      snprintf(rows[n++].value, sizeof(rows[0].value), "%02d:00", prefs.nightStart);
      ids[n] = R_NEND;
      rows[n] = {"Night ends", "", ui::ROW_STEPPER};
      snprintf(rows[n++].value, sizeof(rows[0].value), "%02d:00", prefs.nightEnd);
      ids[n] = R_NDIM;
      rows[n] = {"Night dim", "", ui::ROW_STEPPER};
      snprintf(rows[n++].value, sizeof(rows[0].value), "%d%%", prefs.nightBright);
    } else if (page == 3) {
      // whose glucose the screen shows: the account's own and the people it
      // follows, up to four at once; nothing chosen yet reads as the default
      if (!prefs.people[0]) {
        const char *chosen[SS_MAX_TILES];
        const int c = chosen_ids(chosen);
        for (int i = 0; i < c; i++) ss::selectionToggle(prefs.people, sizeof(prefs.people), chosen[i]);
      }
      const int maxRows = ui::menuLayout().maxRows;
      if (own_has_source()) {
        ids[n] = R_PERSON;
        who[n] = ME;
        rows[n] = {"Me", "", ui::ROW_CYCLE};
        snprintf(rows[n++].value, sizeof(rows[0].value), "%s", ss::selectionHas(prefs.people, ME) ? "On" : "Off");
      }
      for (int i = 0; i < snap.nfollowed && n < maxRows; i++) {
        ids[n] = R_PERSON;
        who[n] = snap.followed[i].id;
        rows[n] = {snap.followed[i].label, "", ui::ROW_CYCLE};
        snprintf(rows[n++].value, sizeof(rows[0].value), "%s", ss::selectionHas(prefs.people, snap.followed[i].id) ? "On" : "Off");
      }
    } else {
      // A newer firmware leads the page while there is one.
      if (update_available()) {
        ids[n] = R_UPDATE;
        rows[n] = {"Update", "", ui::ROW_ACTION};
        snprintf(rows[n++].value, sizeof(rows[0].value), "%s available", snap.latest);
      }
      // Auto = the clock follows the phone that paired the display; Manual
      // opens the offset row under it.
      ids[n] = R_ZONE;
      rows[n] = {"Time zone", "", ui::ROW_CYCLE};
      if (prefs.tzAuto) snprintf(rows[n].value, sizeof(rows[0].value), "Auto (%s)", offset);
      else snprintf(rows[n].value, sizeof(rows[0].value), "Manual");
      n++;
      if (!prefs.tzAuto) {
        ids[n] = R_OFFSET;
        rows[n] = {"UTC offset", "", ui::ROW_STEPPER};
        snprintf(rows[n++].value, sizeof(rows[0].value), "%s", offset);
      }
      ids[n] = R_WIFI;
      rows[n] = {"Wi-Fi", "", ui::ROW_ACTION};
      if (net_wifi_up()) ui::asciiLabel(prefs.ssid, rows[n].value, sizeof(rows[0].value));
      else snprintf(rows[n].value, sizeof(rows[0].value), "Not connected");
      n++;
      if (prefs.token[0]) {
        ids[n] = R_DISCONNECT;
        rows[n++] = {"Disconnect", "", ui::ROW_ACTION};
      }
    }
    const uint32_t t0 = millis();
    ui::drawMenu(PAGES[page], page == 2 ? "v" FW_VERSION : (page == 3 ? "Up to 4 at once" : nullptr), rows, n, "Done", PAGES[(page + 1) % npages]);
    const uint32_t t1 = millis();
    hw_present();
    last_draw_ms = t1 - t0;
    last_flush_ms = millis() - t1;
    trace(TR_FRAME, (int)last_draw_ms, (int)last_flush_ms);
    wait_tap();
    trace(TR_TAP, tap_x, tap_y);
    tick();
    ui::MenuLayout L = ui::menuLayout();
    if (L.left().has(tap_x, tap_y)) {
      const uint32_t s0 = millis();
      store_save();
      trace(TR_SAVE, (int)(millis() - s0), 0);
      return false;
    }
    if (L.right().has(tap_x, tap_y)) {
      page = (page + 1) % npages;
      hw_backlight(prefs.bright);
      continue;
    }
    int row = -1, dir = 0;
    for (int i = 0; i < n; i++) {
      if (!L.row(i).has(tap_x, tap_y)) continue;
      row = i;
      if (L.minus(i).has(tap_x, tap_y)) dir = -1;
      else if (L.plus(i).has(tap_x, tap_y)) dir = 1;
    }
    if (row < 0) continue;
    bool preview_night = false;
    switch (ids[row]) {
      case R_BRIGHT: prefs.bright = constrain(prefs.bright + dir * 10, 20, 100); break;
      case R_SCENE: prefs.scene = (prefs.scene + 1) % 4; break;
      case R_GRAPH:
        // 3, 6, 12, 24 hours, then Off (numbers only)
        prefs.graphHours = prefs.graphHours == 3 ? 6 : (prefs.graphHours == 6 ? 12 : (prefs.graphHours == 12 ? 24 : (prefs.graphHours == 24 ? 0 : 3)));
        net_ask_for(true);   // the new window's readings are on their way before Settings closes
        break;
      case R_ROTATE:
        prefs.rot = (prefs.rot + 1) & 3;
        gx::setRotation(prefs.rot);
        break;
      case R_NSTART: prefs.nightStart = (prefs.nightStart + 24 + dir) % 24; break;
      case R_NEND: prefs.nightEnd = (prefs.nightEnd + 24 + dir) % 24; break;
      case R_NDIM:
        prefs.nightBright = constrain(prefs.nightBright + dir * 5, 5, 100);
        preview_night = dir != 0;   // show the level being chosen
        break;
      case R_UPDATE: {
        // where the new version is, without the "https://"
        const char *where = snap.installUrl[0] ? snap.installUrl : SS_INSTALL_URL;
        ui::drawUpdate(snap.latest, strncmp(where, "https://", 8) == 0 ? where + 8 : where);
        hw_present();
        hw_touch_flush();
        wait_tap();
        tick();
        break;
      }
      case R_ZONE:
        prefs.tzAuto = !prefs.tzAuto;
        if (prefs.tzAuto) adopt_offset(snap.hasOffset, snap.offsetMin);   // back to the phone's zone at once
        break;
      case R_OFFSET: prefs.tzMin = (int16_t)constrain(prefs.tzMin + dir * 30, -720, 840); break;
      case R_SOUND: prefs.sound = !prefs.sound; break;
      case R_PERSON:
        // the last chosen person stays, a fifth is refused (the rule in ss_logic.h)
        if (who[row] && ss::selectionToggle(prefs.people, sizeof(prefs.people), who[row])) {
          rebuild_tiles();
          net_ask_for(true);   // the newly chosen person's reading is on its way before Settings closes
          dirty = true;
        }
        break;
      case R_WIFI:
        store_save();
        flow_wifi(false);
        break;
      case R_DISCONNECT:
        if (confirm_disconnect()) {
          // end the session on the server too, then forget it here
          ui::drawSplash("Disconnecting" CH_ELL);
          hw_present();
          net_pause();
          if (net_wifi_up()) api_unpair(prefs.token);
          prefs.token[0] = '\0';
          store_save();
          snap = ui::Snapshot();
          snap_seq++;
          net_resume();
          return true;
        }
        break;
    }
    hw_backlight(preview_night ? prefs.nightBright : prefs.bright);
  }
}

// ---- pairing ----------------------------------------------------------------------
// Shows a code and a QR code of the pairing page with that code inside, waits
// for the person to confirm it there, stores the session token. A code lives
// ten minutes; a fresh one (and its QR code) follows by itself.
static void flow_pairing_inner() {
  static PairStart ps;
  memset(&ps, 0, sizeof(ps));
  uint32_t code_until = 0, next_poll = 0, next_start = 0, drawn_s = 0xFFFFFFFF;
  const char *note = nullptr;
  qr_n = 0;
  qr_app_url[0] = '\0';   // the buffer is the pairing code's from here
  Serial.println("pairing: waiting for a code");
  for (;;) {
    bool have = ps.code[0] && (int32_t)(code_until - millis()) > 0;
    if (!have && (int32_t)(next_start - millis()) <= 0) {
      ps.code[0] = '\0';
      if (!net_wifi_up()) {
        note = "No Wi-Fi";
        next_start = millis() + 3000;
      } else {
        int rc = api_pair_start(ps);
        if (rc == 200) {
          qr_make(ps.url);
          code_until = millis() + (uint32_t)ps.expiresS * 1000 - 5000;
          next_poll = millis() + (uint32_t)ps.pollS * 1000;
          note = nullptr;
          api_ok = true;
          have = true;
          Serial.println("pairing: a code is on the screen");
        } else {
          ps.code[0] = '\0';
          api_ok = false;
          note = rc == 429 ? "Too many tries, waiting" CH_ELL : (rc == 503 ? "Pairing is switched off" : "Can't reach Sugar Sense" CH_ELL);
          next_start = millis() + (rc == 429 ? 60000 : 8000);
          Serial.printf("pairing: start failed (%d)\n", rc);
        }
      }
      drawn_s = 0xFFFFFFFF;
    }
    if (have && (int32_t)(next_poll - millis()) <= 0) {
      bool pending = true, has_offset = false;
      int offset_min = 0;
      char token[48] = "";
      int rc = api_pair_poll(ps.secret, &pending, token, sizeof(token), &has_offset, &offset_min);
      next_poll = millis() + (uint32_t)ps.pollS * 1000;
      if (rc == 200 && !pending) {
        snprintf(prefs.token, sizeof(prefs.token), "%s", token);
        // the clock of the phone that confirmed the code, from the first frame
        if (has_offset && prefs.tzAuto) prefs.tzMin = (int16_t)constrain(offset_min, -720, 840);
        store_save();
        chime();
        Serial.println("pairing: paired");
        return;
      }
      if (rc == 404) {   // expired or already used: a fresh code
        ps.code[0] = '\0';
        next_start = millis();
      } else if (rc != 200) {
        api_ok = false;
        note = "Can't reach Sugar Sense" CH_ELL;
      } else {
        api_ok = true;
        note = nullptr;
      }
      drawn_s = 0xFFFFFFFF;
    }
    uint32_t s = millis() / 1000;
    if (s != drawn_s) {   // once a second, for the countdown
      drawn_s = s;
      have = ps.code[0] && (int32_t)(code_until - millis()) > 0;
      int left = have ? (int)((code_until + 5000 - millis()) / 1000) : -1;
      ui::drawPairing(make_view(), have ? ps.code : "", left, qr_mods, qr_n, ps.label[0] ? ps.label : SS_PAIR_URL_LABEL, note);
      hw_present();
      hw_backlight(prefs.bright);
    }
    if (hw_tap() && ui::settingsButton().has(tap_x, tap_y)) {
      tick();
      flow_settings();
      hw_touch_flush();
      drawn_s = 0xFFFFFFFF;
    }
    service_info();
    delay(5);
  }
}

static void flow_pairing() {
  net_pause();
  flow_pairing_inner();
  net_resume();
}

// ---- the home screen ----------------------------------------------------------------

// Everything the home screen shows that can change without a tap.
static uint32_t home_signature() {
  int64_t now = clock_now_ms();
  int h, m;
  ss::localHourMinute(now, prefs.tzMin, &h, &m);
  uint32_t sig = (uint32_t)(h * 60 + m);
  sig = sig * 31 + (uint32_t)ss::elapsedMinutes(now, snap.dateMs);
  for (int i = 0; i < ntiles; i++) sig = sig * 31 + (uint32_t)ss::elapsedMinutes(now, tiles[i].snap->dateMs) + (ss::isStale(now, tiles[i].snap->dateMs) ? 7 : 0);
  sig = sig * 31 + (uint32_t)ntiles;
  sig = sig * 31 + (net_wifi_up() ? 1 : 0) + (api_ok ? 2 : 0) + (clock_synced() ? 4 : 0) + (ss::isStale(now, snap.dateMs) ? 8 : 0) +
        (ss::isLongStale(now, snap.dateMs) ? 16 : 0) + (uint32_t)current_look() * 32;
  sig = sig * 31 + snap_seq;
  return sig;
}

static void render_home() {
  const uint32_t t0 = millis();
  const int look = current_look();
  ui::updateHint = update_available() && look != ui::LOOK_NIGHT;   // a quiet dot by day, nothing at night
  ui::View v = make_view();
  if (ntiles > 1) {
    ui::drawSplit(v, look, tiles, ntiles);   // several people at once
  } else if (ntiles == 1 && !tiles[0].self) {
    // one followed person alone: the whole scene with their name, or the
    // tile that says why there is no number
    const ui::TileSource &t = tiles[0];
    if (t.snap->state == ss::STATE_OK && !t.notShared && !t.paused) {
      v.snap = t.snap;
      v.personLabel = t.label;
      v.graphHours = t.snap->hours ? t.snap->hours : v.graphHours;
      ui::drawHome(v, look);
    } else {
      ui::drawSplit(v, look, tiles, 1);
    }
  } else {
    ui::drawHome(v, look);
  }
  ui::updateHint = false;
  const uint32_t t1 = millis();
  hw_present();
  last_draw_ms = t1 - t0;
  last_flush_ms = millis() - t1;
  trace(TR_FRAME, (int)last_draw_ms, (int)last_flush_ms);
}

static void home_tap() {
  trace(TR_TAP, tap_x, tap_y);
  tick();
  dirty = true;
  if (ui::settingsButton().has(tap_x, tap_y)) {
    if (flow_settings()) need_pair = true;
    hw_touch_flush();
    return;
  }
  // The home screen is for looking, not for controls: a touch anywhere else
  // only brings the light up for a minute (the graph's window is in Settings).
  wake_until_ms = millis() + 60000;
  dirty = false;   // nothing on the screen changed
}

static uint32_t stage_micros() { return micros(); }

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // a log line never waits for a computer that is not listening
  ui::stageClock = stage_micros;
  store_load();
  bool ok = hw_display_begin();
  gx::setRotation(prefs.rot);
  hw_touch_begin();
  hw_sound_begin();
  Serial.printf("\n=== sugarsense-display v%s ===\n", FW_VERSION);
  if (!ok) {
    for (;;) {
      Serial.println("display init failed");
      delay(2000);
    }
  }
  ui::drawSplash(nullptr);
  hw_present();
  hw_backlight(prefs.bright);

  WiFi.mode(WIFI_STA);
  net_begin();
  net_lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task, "net", 16384, nullptr, 1, &net_task_handle, 0);
  if (!prefs.ssid[0]) {
    flow_wifi(true);
  } else {
    ui::drawSplash("Connecting to Wi-Fi" CH_ELL);
    hw_present();
    WiFi.setAutoReconnect(true);
    WiFi.begin(prefs.ssid, prefs.wpass);
    for (uint32_t t0 = millis(); millis() - t0 < 15000 && !net_wifi_up();) delay(200);
  }
  Serial.printf("wifi: %s\n", net_wifi_up() ? "up" : "down");
  if (!prefs.token[0]) flow_pairing();
  net_resume();   // the pulls start here
}

void loop() {
  static uint32_t last_reconnect = 0, last_sig = 0;
  if (need_pair) {
    need_pair = false;
    flow_pairing();
    dirty = true;
  }
  if (!net_wifi_up() && prefs.ssid[0] && millis() - last_reconnect > 30000) {
    last_reconnect = millis();
    WiFi.reconnect();
  }
  net_collect();
  apply_backlight();
  uint32_t sig = home_signature();
  if (dirty || sig != last_sig) {
    dirty = false;
    last_sig = sig;
    render_home();
  }
  if (hw_tap()) home_tap();
  service_info();
  delay(5);
}
