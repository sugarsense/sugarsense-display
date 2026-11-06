// The board under the display: a Guition JC3248W535 (ESP32-S3, a 3.5 inch
// 320x480 panel with an AXS15231B controller over QSPI, capacitive touch on
// the same chip, an I2S amplifier for a small speaker).
//
// The panel is wired the way the Arduino_GFX library itself lists this board
// (its JC3248W535 device entry): QSPI on 45 / 47 / 21 / 48 / 40 / 39, the
// backlight on GPIO 1, and a full-frame canvas because the controller has no
// rotation of its own. Rotation is done in gx.h.
#ifndef SS_HW_H
#define SS_HW_H

#include <Arduino_GFX_Library.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <mbedtls/base64.h>
#include "gx.h"

// ---- a trace of what happened when ------------------------------------------------
// The last 64 events with their time, written from any task and printed by the
// USB console's `trace` command: the way to see where a slow moment went.
enum { TR_TOUCH = 1, TR_TAP, TR_FRAME, TR_SAVE, TR_PULL, TR_ANSWER };
struct TraceEvent {
  uint32_t ms;
  uint8_t kind;
  int16_t a, b;
};
static TraceEvent trace_events[64];
static volatile uint32_t trace_count = 0;
static portMUX_TYPE trace_mux = portMUX_INITIALIZER_UNLOCKED;

static void trace(uint8_t kind, int a, int b) {
  portENTER_CRITICAL(&trace_mux);
  TraceEvent &e = trace_events[trace_count % 64];
  e.ms = millis();
  e.kind = kind;
  e.a = (int16_t)a;
  e.b = (int16_t)b;
  trace_count = trace_count + 1;
  portEXIT_CRITICAL(&trace_mux);
}

static void trace_print() {
  static const char *NAMES[] = {"?", "touch", "tap", "frame", "save", "pull", "answer"};
  const uint32_t n = trace_count, from = n > 64 ? n - 64 : 0;
  Serial.setTxTimeoutMs(250);
  for (uint32_t i = from; i < n; i++) {
    const TraceEvent &e = trace_events[i % 64];
    Serial.printf("TRACE %lu %s %d %d\n", (unsigned long)e.ms, NAMES[e.kind < 7 ? e.kind : 0], e.a, e.b);
  }
  Serial.println("TRACE END");
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}

// ---- the panel ----------------------------------------------------------------
static const int PIN_BACKLIGHT = 1;
static Arduino_DataBus *hw_bus = new Arduino_ESP32QSPI(45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);
static Arduino_GFX *hw_panel = new Arduino_AXS15231B(hw_bus, GFX_NOT_DEFINED /* RST */, 0, false /* IPS */, gx::NATIVE_W, gx::NATIVE_H);
static Arduino_Canvas *hw_canvas = new Arduino_Canvas(gx::NATIVE_W, gx::NATIVE_H, hw_panel, 0, 0, 0);

static int hw_backlight_now = -1;

// Brightness in percent, PWM on the backlight pin.
static void hw_backlight(int percent) {
  percent = constrain(percent, 0, 100);
  if (percent == hw_backlight_now) return;
  if (hw_backlight_now < 0) ledcAttach(PIN_BACKLIGHT, 5000 /* Hz */, 8 /* bits */);
  ledcWrite(PIN_BACKLIGHT, 255 * percent / 100);
  hw_backlight_now = percent;
}

// Dark until the first frame is on the glass.
static bool hw_display_begin() {
  hw_backlight(0);
  if (!hw_canvas->begin(40000000)) return false;
  gx::fb = hw_canvas->getFramebuffer();
  return gx::fb != nullptr;
}

// Push the frame the app just drew.
static void hw_present() { hw_canvas->flush(); }

// ---- touch ----------------------------------------------------------------------
// The panel controller answers on I2C (SDA 4, SCL 8, address 0x3B) and pulls
// its interrupt line (GPIO 3) low when it has a report: a finger landed,
// moved or left. One request returns the number of fingers and the first
// finger's position in the panel's own portrait frame.
//
// The controller is read ONLY when it says it has a report. Asking it on a
// timer instead (tried: a hundred times a second) makes it lose most touches.
//
// A small task of its own does the reading, the moment the report comes and
// whatever the app is busy with, and queues the taps. A TAP is a finger
// report on glass that was free. Measured on the board: while a finger is
// down the controller reports about every 15 ms, and it sends NO report when
// the finger leaves, so the glass counts as free after 80 ms without a finger
// report. A resting finger keeps reporting and stays one tap; two quick taps
// are two.
static const int TOUCH_SDA = 4, TOUCH_SCL = 8, TOUCH_IRQ = 3;
static const uint8_t TOUCH_ADDRESS = 0x3B;
static const uint32_t TOUCH_QUIET_MS = 80;    // no finger report for this long = the finger is gone
static const uint32_t TOUCH_BOUNCE_MS = 30;   // a landing this soon after a finger report is the same touch

struct TouchTap {
  int16_t x, y;
};

static QueueHandle_t touch_queue = nullptr;
static volatile bool touch_pending = false;
static volatile uint32_t touch_taps = 0;                               // taps since boot
static volatile uint32_t touch_finger = 0, touch_none = 0, touch_bad = 0;   // what the reads returned, for the status line

static void IRAM_ATTR touch_on_irq() { touch_pending = true; }

// Ask the controller where the finger is: 1 = a finger (x, y filled), 0 = no
// finger, -1 = the read failed or made no sense.
static int touch_read(int *x, int *y) {
  static const uint8_t request[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};
  uint8_t reply[8] = {0};
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(request, sizeof(request));
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom(TOUCH_ADDRESS, (uint8_t)sizeof(reply)) != sizeof(reply)) return -1;
  Wire.readBytes(reply, sizeof(reply));
  const uint8_t fingers = reply[1];
  if (fingers == 0) return 0;
  if (fingers > 5) return -1;
  *x = ((reply[2] & 0x0F) << 8) | reply[3];
  *y = ((reply[4] & 0x0F) << 8) | reply[5];
  return (*x < gx::NATIVE_W && *y < gx::NATIVE_H) ? 1 : -1;
}

static void touch_task(void *) {
  bool down = false;
  uint32_t last_finger = 0;   // the last moment the controller reported a finger
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
    const uint32_t now = millis();
    if (down && now - last_finger > TOUCH_QUIET_MS) down = false;
    // a new report, or one still waiting behind a line held low
    if (!touch_pending && digitalRead(TOUCH_IRQ) != LOW) continue;
    touch_pending = false;
    int x = 0, y = 0;
    const int r = touch_read(&x, &y);
    if (r < 0) {
      touch_bad = touch_bad + 1;   // says nothing either way
      continue;
    }
    if (r == 0) {
      touch_none = touch_none + 1;
      down = false;   // the finger left
      continue;
    }
    touch_finger = touch_finger + 1;
    const bool landed = !down && (last_finger == 0 || now - last_finger >= TOUCH_BOUNCE_MS);
    down = true;
    last_finger = now ? now : 1;
    if (!landed) continue;
    trace(TR_TOUCH, x, y);
    touch_taps = touch_taps + 1;
    TouchTap tap = {(int16_t)x, (int16_t)y};
    xQueueSend(touch_queue, &tap, 0);
  }
}

static void hw_touch_begin() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  touch_queue = xQueueCreate(6, sizeof(TouchTap));
  attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ), touch_on_irq, FALLING);
  xTaskCreatePinnedToCore(touch_task, "touch", 3072, nullptr, 3, nullptr, 1);
}

// The next tap, if one is waiting; the point is in the panel's portrait frame.
static bool hw_touch_tap(int *x, int *y) {
  TouchTap tap;
  if (!touch_queue || xQueueReceive(touch_queue, &tap, 0) != pdTRUE) return false;
  *x = tap.x;
  *y = tap.y;
  return true;
}

// Forget the taps that were made for the screen that just went away.
static void hw_touch_flush() {
  if (touch_queue) xQueueReset(touch_queue);
}

// ---- sound ------------------------------------------------------------------------
// The I2S amplifier (BCLK 42, LRCLK 2, DOUT 41) drives a small speaker when one
// is plugged in. The display only makes soft interface sounds: a tap tick and
// a two-note chime. Each note is a sine with a few milliseconds of fade at
// both ends, so it never clicks.
static const int SOUND_RATE = 16000;
static I2SClass hw_i2s;
static bool hw_sound_ready = false;

static void hw_sound_begin() {
  hw_i2s.setPins(42, 2, 41);
  hw_sound_ready = hw_i2s.begin(I2S_MODE_STD, SOUND_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
}

// `level` 0..100.
static void hw_note(int hertz, int milliseconds, int level) {
  if (!hw_sound_ready || level <= 0) return;
  const int total = SOUND_RATE * milliseconds / 1000;
  const int fade = min(total / 3, SOUND_RATE / 200);
  const float step = 6.2831853f * hertz / SOUND_RATE, peak = 280.0f * level;
  int16_t block[160];
  float phase = 0.0f;
  for (int done = 0; done < total;) {
    int n = min((int)(sizeof(block) / sizeof(block[0])), total - done);
    for (int i = 0; i < n; i++, done++) {
      float gain = done < fade ? (float)done / fade : (total - done < fade ? (float)(total - done) / fade : 1.0f);
      block[i] = (int16_t)(sinf(phase) * peak * gain);
      phase += step;
    }
    hw_i2s.write((const uint8_t *)block, n * sizeof(block[0]));
  }
}

// ---- input: the touch panel, plus a small USB serial console ------------------
// Over the USB cable the board answers four line commands, for checking a
// build from a computer without looking at the glass (tools/devtool.py):
//   shot          the current frame, base64 RGB565 between SHOT / END lines
//   tap <x> <y>   a tap at logical coordinates
//   info          one status line (printed by the app)
//   trace         the last 64 events with their time
// It needs the cable in hand; nothing here is reachable over the network.
static int tap_x = 0, tap_y = 0;
static bool hw_info_requested = false;

static void hw_serial_shot() {
  Serial.setTxTimeoutMs(250);   // the one transfer that waits for the computer
  Serial.printf("SHOT %d %d %d\n", gx::NATIVE_W, gx::NATIVE_H, gx::rot);
  const uint8_t *raw = (const uint8_t *)gx::fb;
  const size_t total = (size_t)gx::NATIVE_W * gx::NATIVE_H * 2;
  static unsigned char out[4100];
  for (size_t off = 0; off < total; off += 3072) {
    size_t n = total - off < 3072 ? total - off : 3072, olen = 0;
    mbedtls_base64_encode(out, sizeof(out), &olen, raw + off, n);
    Serial.write(out, olen);
    Serial.write('\n');
  }
  Serial.println("END");
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}

static bool hw_serial_service() {
  static char line[40];
  static int len = 0;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < (int)sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = '\0';
    len = 0;
    int x, y;
    if (strcmp(line, "shot") == 0) hw_serial_shot();
    else if (strcmp(line, "trace") == 0) trace_print();
    else if (strcmp(line, "info") == 0) hw_info_requested = true;
    else if (sscanf(line, "tap %d %d", &x, &y) == 2) {
      tap_x = x;
      tap_y = y;
      return true;   // one tap per call: taps sent in a burst queue up instead of collapsing into the last
    }
  }
  return false;
}

// True once per tap; the point is in tap_x / tap_y, logical coordinates.
static bool hw_tap() {
  if (hw_serial_service()) return true;
  int nx, ny;
  if (!hw_touch_tap(&nx, &ny)) return false;
  gx::fromNative(nx, ny, &tap_x, &tap_y);
  return true;
}

#endif
