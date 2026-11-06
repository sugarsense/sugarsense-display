// A small software renderer over the panel's RGB565 framebuffer: filled and
// rounded rectangles, discs, lines, a soft glow and 4-bit alpha text, all
// anti-aliased by blending against the pixel already there. Every shape takes
// an opacity, so a "glass" card over a colored canvas is one call.
//
// The framebuffer lives in slow external RAM: large fills walk it in the
// order it lies in memory (see rect), or a frame takes three times as long.
//
// Pure C++ over a raw buffer, so the SAME drawing code runs on the board
// (the buffer is Arduino_Canvas's) and on a computer (tools/host_test.py
// renders every screen to a PNG). Rotation is handled here, in one pixel
// mapping; the canvas itself always stays at rotation 0.
#ifndef SS_GX_H
#define SS_GX_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "assets.h"

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define HEX(h) RGB(((h) >> 16) & 0xFF, ((h) >> 8) & 0xFF, (h) & 0xFF)

namespace gx {

static const int NATIVE_W = 320, NATIVE_H = 480;
static uint16_t *fb = nullptr;   // NATIVE_W x NATIVE_H, the panel's own order
static int rot = 0;              // 0 portrait, 1 / 3 landscape, 2 portrait flipped
static int W = NATIVE_W, H = NATIVE_H;

static inline void setRotation(int r) {
  rot = r & 3;
  W = (rot & 1) ? NATIVE_H : NATIVE_W;
  H = (rot & 1) ? NATIVE_W : NATIVE_H;
}

// Logical (x, y) -> the framebuffer cell. The caller guarantees the bounds.
static inline uint16_t *at(int x, int y) {
  switch (rot) {
    case 1: return fb + x * NATIVE_W + (NATIVE_W - 1 - y);
    case 2: return fb + (NATIVE_H - 1 - y) * NATIVE_W + (NATIVE_W - 1 - x);
    case 3: return fb + (NATIVE_H - 1 - x) * NATIVE_W + y;
    default: return fb + y * NATIVE_W + x;
  }
}

// A raw touch point (the panel's portrait frame) -> logical coordinates.
static inline void fromNative(int nx, int ny, int *x, int *y) {
  switch (rot) {
    case 1: *x = ny; *y = NATIVE_W - 1 - nx; break;
    case 2: *x = NATIVE_W - 1 - nx; *y = NATIVE_H - 1 - ny; break;
    case 3: *x = NATIVE_H - 1 - ny; *y = nx; break;
    default: *x = nx; *y = ny;
  }
}

// fg over bg at alpha 0..255. The alpha is stretched to 0..256 so the blend
// is three multiplies and a shift (this runs for every anti-aliased pixel).
static inline uint16_t mix(uint16_t fg, uint16_t bg, int a) {
  a += a >> 7;
  const int ia = 256 - a;
  int r = ((fg >> 11) * a + (bg >> 11) * ia + 128) >> 8;
  int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * ia + 128) >> 8;
  int b = ((fg & 31) * a + (bg & 31) * ia + 128) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void blend(int x, int y, uint16_t c, int a) {
  if (a <= 0 || x < 0 || y < 0 || x >= W || y >= H) return;
  uint16_t *p = at(x, y);
  *p = a >= 255 ? c : mix(c, *p, a);
}

// A 4x4 ordered dither spreads the rounding to 5 / 6 / 5 bits, so a faint
// gradient on a dark screen does not break into bands.
static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

// fg over bg at t 0..1, without dither (for a color, not a gradient).
static inline uint16_t lerp(uint16_t a, uint16_t b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  return mix(b, a, (int)(t * 255.0f + 0.5f));
}

// The same blend for soft gradients (a glow, a fade), dithered.
static inline void blendSoft(int x, int y, uint16_t c, int a) {
  if (a <= 0 || x < 0 || y < 0 || x >= W || y >= H) return;
  const int d = BAYER[(y & 3) * 4 + (x & 3)];
  a += a >> 7;
  const int ia = 256 - a;
  uint16_t *p = at(x, y);
  const uint16_t bg = *p;
  // each channel in 1/16 steps of its own scale, the dither added before the cut
  int r = ((((c >> 11) * a + (bg >> 11) * ia) >> 4) + d) >> 4;
  int g = (((((c >> 5) & 63) * a + ((bg >> 5) & 63) * ia) >> 4) + d) >> 4;
  int b = ((((c & 31) * a + (bg & 31) * ia) >> 4) + d) >> 4;
  *p = (uint16_t)(r << 11 | g << 5 | b);
}

// Signed distance to an edge in pixels (negative inside) -> coverage 0..255.
static inline int cover(float d) {
  float a = 0.5f - d;
  if (a <= 0.0f) return 0;
  if (a >= 1.0f) return 255;
  return (int)(a * 255.0f + 0.5f);
}

static void clear(uint16_t c) {
  for (int i = 0; i < NATIVE_W * NATIVE_H; i++) fb[i] = c;
}

// `a` is the opacity 0..255 of the whole shape, here and in every shape below.
static void rect(int x, int y, int w, int h, uint16_t c, int a = 255) {
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > W) x1 = W;
  if (y1 > H) y1 = H;
  if (a <= 0) return;
  // The framebuffer lives in slow external RAM. Turned on its side, a logical
  // column is what lies contiguous in memory, so the inner loop runs along it
  // (the other order misses the cache on every pixel).
  if (rot & 1) {
    for (int xx = x; xx < x1; xx++) {
      for (int yy = y; yy < y1; yy++) {
        uint16_t *p = at(xx, yy);
        *p = a >= 255 ? c : mix(c, *p, a);
      }
    }
    return;
  }
  for (int yy = y; yy < y1; yy++) {
    for (int xx = x; xx < x1; xx++) {
      uint16_t *p = at(xx, yy);
      *p = a >= 255 ? c : mix(c, *p, a);
    }
  }
}

// The three bands and the four corners never overlap, so a translucent
// rounded rectangle has one even tone.
static void rrect(int x, int y, int w, int h, int r, uint16_t c, int a = 255) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  rect(x + r, y, w - 2 * r, h, c, a);
  rect(x, y + r, r, h - 2 * r, c, a);
  rect(x + w - r, y + r, r, h - 2 * r, c, a);
  for (int j = 0; j < r; j++) {
    for (int i = 0; i < r; i++) {
      float dx = r - (i + 0.5f), dy = r - (j + 0.5f);
      int k = cover(sqrtf(dx * dx + dy * dy) - r) * a / 255;
      if (!k) continue;
      blend(x + i, y + j, c, k);
      blend(x + w - 1 - i, y + j, c, k);
      blend(x + i, y + h - 1 - j, c, k);
      blend(x + w - 1 - i, y + h - 1 - j, c, k);
    }
  }
}

static void disc(float cx, float cy, float r, uint16_t c, int a = 255) {
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
  int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
  const float in = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0.0f, out = (r + 0.5f) * (r + 0.5f);
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
      if (d2 >= out) continue;
      blend(x, y, c, d2 <= in ? a : cover(sqrtf(d2) - r) * a / 255);   // the root only on the rim
    }
  }
}

// A soft glow in an ellipse: `peak` opacity at the centre, fading to nothing
// at the rim. `yStop` cuts it off where something opaque is drawn later. It
// can cover half the screen, so the falloff is (1 - q)^2 on the squared
// distance: no square root and no division per pixel.
static void glow(float cx, float cy, float rx, float ry, uint16_t c, int peak, int yStop = 1 << 20) {
  int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
  if (y0 < 0) y0 = 0;
  if (y1 >= H) y1 = H - 1;
  if (y1 >= yStop) y1 = yStop - 1;
  const float ix = 1.0f / rx, iy = 1.0f / ry;
  if (rot & 1) {   // on its side: column by column, the order memory lies in
    int xa = (int)floorf(cx - rx), xb = (int)ceilf(cx + rx);
    if (xa < 0) xa = 0;
    if (xb >= W) xb = W - 1;
    for (int x = xa; x <= xb; x++) {
      const float nx = (x + 0.5f - cx) * ix, qx = nx * nx;
      if (qx >= 1.0f) continue;
      const float half = ry * sqrtf(1.0f - qx);
      int ya = (int)floorf(cy - half), yb = (int)ceilf(cy + half);
      if (ya < y0) ya = y0;
      if (yb > y1) yb = y1;
      for (int y = ya; y <= yb; y++) {
        const float ny = (y + 0.5f - cy) * iy, f = 1.0f - (qx + ny * ny);
        if (f <= 0.0f) continue;
        blendSoft(x, y, c, (int)(peak * f * f + 0.5f));
      }
    }
    return;
  }
  for (int y = y0; y <= y1; y++) {
    const float ny = (y + 0.5f - cy) * iy, qy = ny * ny;
    if (qy >= 1.0f) continue;
    const float half = rx * sqrtf(1.0f - qy);   // one root per row: where the ellipse ends
    int x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    for (int x = x0; x <= x1; x++) {
      const float nx = (x + 0.5f - cx) * ix, f = 1.0f - (nx * nx + qy);
      if (f <= 0.0f) continue;
      blendSoft(x, y, c, (int)(peak * f * f + 0.5f));
    }
  }
}

// One pixel column fading from `top` opacity at y0 down to nothing at y1:
// the soft fill under a trend line.
static void fadeDown(int x, int y0, int y1, uint16_t c, int top) {
  if (y1 <= y0) return;
  for (int y = y0; y < y1; y++) blendSoft(x, y, c, top * (y1 - y) / (y1 - y0));
}

// A round-capped segment. Meant for short strokes: the cost is its bounding
// box. Pixels well inside or outside are decided on the squared distance; the
// square root runs only on the edge.
static void line(float ax, float ay, float bx, float by, float thick, uint16_t c) {
  const float h = thick * 0.5f;
  int x0 = (int)floorf(fminf(ax, bx) - h - 1), x1 = (int)ceilf(fmaxf(ax, bx) + h + 1);
  int y0 = (int)floorf(fminf(ay, by) - h - 1), y1 = (int)ceilf(fmaxf(ay, by) + h + 1);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= W) x1 = W - 1;
  if (y1 >= H) y1 = H - 1;
  const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy, inv = len2 > 0.0f ? 1.0f / len2 : 0.0f;
  const float in = h > 0.5f ? (h - 0.5f) * (h - 0.5f) : 0.0f, out = (h + 0.5f) * (h + 0.5f);
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      const float px = x + 0.5f - ax, py = y + 0.5f - ay;
      float t = (px * vx + py * vy) * inv;
      if (t < 0.0f) t = 0.0f;
      if (t > 1.0f) t = 1.0f;
      const float dx = px - t * vx, dy = py - t * vy, d2 = dx * dx + dy * dy;
      if (d2 >= out) continue;
      if (d2 <= in) *at(x, y) = c;
      else blend(x, y, c, cover(sqrtf(d2) - h));
    }
  }
}

static void dashH(int x, int y, int w, int on, int off, uint16_t c, int a = 255) {
  for (int i = 0; i < w; i += on + off) rect(x + i, y, (i + on > w) ? (w - i) : on, 1, c, a);
}

// `spacing` adds that many pixels after every glyph (small capitals breathe).
static int textW(const AAFont &f, const char *s, int spacing = 0) {
  int w = 0;
  for (; *s; s++) {
    uint8_t ch = (uint8_t)*s;
    if (ch < f.first || ch > f.last) continue;
    w += f.gl[ch - f.first].adv + spacing;
  }
  return w > 0 ? w - spacing : 0;
}

// Draws from the BASELINE; returns the x after the last glyph.
static int text(const AAFont &f, int x, int base, const char *s, uint16_t c, int alpha = 255, int spacing = 0) {
  for (; *s; s++) {
    uint8_t ch = (uint8_t)*s;
    if (ch < f.first || ch > f.last) continue;
    const AAGlyph &g = f.gl[ch - f.first];
    const uint8_t *b = f.bmp + g.off;
    int i = 0;
    for (int yy = 0; yy < g.h; yy++) {
      for (int xx = 0; xx < g.w; xx++, i++) {
        uint8_t byte = b[i >> 1];
        int a = (i & 1) ? (byte & 15) : (byte >> 4);
        if (a) blend(x + g.xo + xx, base + g.yo + yy, c, a * 17 * alpha / 255);
      }
    }
    x += g.adv + spacing;
  }
  return x;
}

static void textC(const AAFont &f, int cx, int base, const char *s, uint16_t c, int alpha = 255, int spacing = 0) {
  text(f, cx - textW(f, s, spacing) / 2, base, s, c, alpha, spacing);
}

static void textR(const AAFont &f, int xr, int base, const char *s, uint16_t c, int alpha = 255, int spacing = 0) {
  text(f, xr - textW(f, s, spacing), base, s, c, alpha, spacing);
}

static void image(int x, int y, int w, int h, const uint16_t *px) {
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) blend(x + xx, y + yy, px[yy * w + xx], 255);
}

}  // namespace gx

#endif
