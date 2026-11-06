// The display's settings, kept in the board's non-volatile storage. The Wi-Fi
// password and the Sugar Sense session token live here in plain flash, like on
// every hobby board: whoever holds the device can read them out over USB. The
// token only reads this account's glucose snapshot; no sensor password is
// ever on the device.
#ifndef SS_STORE_H
#define SS_STORE_H

#include <Preferences.h>

// Auto (the default) = the Color scene by day and the Night scene in the night
// hours. The codes are the ones ss::lookForScene reads.
enum { SCENE_AUTO = 0, SCENE_DARK = 1, SCENE_NIGHT = 2, SCENE_COLOR = 3 };

struct Prefs {
  char ssid[33];
  char wpass[65];
  char token[48];        // the session token from pairing, 43 characters
  uint8_t rot;           // 0..3
  uint8_t bright;        // brightness outside the night hours, percent
  uint8_t scene;         // SCENE_AUTO, _DARK, _NIGHT or _COLOR
  uint8_t nightStart;    // local hour the night opens
  uint8_t nightEnd;      // and closes
  uint8_t nightBright;   // brightness inside the night hours, percent
  uint8_t graphHours;    // 3, 6, 12 or 24; 0 = no graph, numbers only
  uint8_t sound;         // interface sounds through the speaker, when one is attached
  int16_t tzMin;         // minutes east of UTC
  uint8_t tzAuto;        // 1 = the offset follows the phone that paired the display (the server sends it)
};

static Prefs prefs;
static Preferences store_nvs;

static void store_load() {
  store_nvs.begin("ssdisplay", false);
  memset(&prefs, 0, sizeof(prefs));
  store_nvs.getString("ssid", prefs.ssid, sizeof(prefs.ssid));
  store_nvs.getString("wpass", prefs.wpass, sizeof(prefs.wpass));
  store_nvs.getString("token", prefs.token, sizeof(prefs.token));
  prefs.rot = store_nvs.getUChar("rot", 0) & 3;
  prefs.bright = store_nvs.getUChar("bright", 100);
  prefs.scene = store_nvs.getUChar("scene", SCENE_AUTO);
  prefs.nightStart = store_nvs.getUChar("nstart", 23);
  prefs.nightEnd = store_nvs.getUChar("nend", 7);
  prefs.nightBright = store_nvs.getUChar("nbright", 10);
  prefs.graphHours = store_nvs.getUChar("ghours", 3);
  prefs.sound = store_nvs.getUChar("sound", 1);
  prefs.tzMin = store_nvs.getShort("tz", 0);
  prefs.tzAuto = store_nvs.getUChar("tzauto", 1);
  if (prefs.scene > SCENE_COLOR) prefs.scene = SCENE_AUTO;
  if (prefs.graphHours != 0 && prefs.graphHours != 3 && prefs.graphHours != 6 && prefs.graphHours != 12 && prefs.graphHours != 24) prefs.graphHours = 3;
}

static void store_save() {
  store_nvs.putString("ssid", prefs.ssid);
  store_nvs.putString("wpass", prefs.wpass);
  store_nvs.putString("token", prefs.token);
  store_nvs.putUChar("rot", prefs.rot);
  store_nvs.putUChar("bright", prefs.bright);
  store_nvs.putUChar("scene", prefs.scene);
  store_nvs.putUChar("nstart", prefs.nightStart);
  store_nvs.putUChar("nend", prefs.nightEnd);
  store_nvs.putUChar("nbright", prefs.nightBright);
  store_nvs.putUChar("ghours", prefs.graphHours);
  store_nvs.putUChar("sound", prefs.sound);
  store_nvs.putShort("tz", prefs.tzMin);
  store_nvs.putUChar("tzauto", prefs.tzAuto);
}

#endif
