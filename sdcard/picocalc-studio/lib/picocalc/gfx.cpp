// gfx.cpp - picocalc.h gfx_*: an off-screen 320x320 RGB565 framebuffer in PSRAM, pushed to the
// panel by gfx_flush() / gfx_flush_rect(). Everything is clipped to the clip rectangle.
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "picocalc.h"
#include "sdk_internal.h"
#include "uifont.h"
#include "font8x8.h"
#include "../hal/hal.h"

static color_t *s_fb;
static int s_cx0, s_cy0, s_cx1 = SCREEN_W, s_cy1 = SCREEN_H;   // clip, x1/y1 exclusive
static int s_font = FONT_6X10;

struct Font { int w, h; const uint8_t *data; bool lsbLeft; };
static const Font kFonts[] = {
  { 6, 10, UIFONT_6X10, false },
  { 7, 13, UIFONT_7X13, false },
  { 8, 8, &FONT_8X8_DATA[0][0], true },
};

void sdk_gfx_init() {
  s_fb = (color_t *)hal_alloc_big(SCREEN_W * SCREEN_H * sizeof(color_t));
}

void sdk_gfx_reset() {
  gfx_noclip();
  s_font = FONT_6X10;
}

static inline void put(int x, int y, color_t c) {
  if (x >= s_cx0 && x < s_cx1 && y >= s_cy0 && y < s_cy1) s_fb[y * SCREEN_W + x] = c;
}

void gfx_clip(int x, int y, int w, int h) {
  s_cx0 = x < 0 ? 0 : x;
  s_cy0 = y < 0 ? 0 : y;
  s_cx1 = x + w > SCREEN_W ? SCREEN_W : x + w;
  s_cy1 = y + h > SCREEN_H ? SCREEN_H : y + h;
  if (s_cx1 < s_cx0) s_cx1 = s_cx0;
  if (s_cy1 < s_cy0) s_cy1 = s_cy0;
}

void gfx_noclip(void) { s_cx0 = s_cy0 = 0; s_cx1 = SCREEN_W; s_cy1 = SCREEN_H; }

void gfx_fill_rect(int x, int y, int w, int h, color_t c) {
  int x0 = x < s_cx0 ? s_cx0 : x, y0 = y < s_cy0 ? s_cy0 : y;
  int x1 = x + w > s_cx1 ? s_cx1 : x + w, y1 = y + h > s_cy1 ? s_cy1 : y + h;
  for (int j = y0; j < y1; j++) {
    color_t *row = s_fb + j * SCREEN_W;
    for (int i = x0; i < x1; i++) row[i] = c;
  }
}

void gfx_clear(color_t c) {
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) s_fb[i] = c;
}

void gfx_pixel(int x, int y, color_t c) { put(x, y, c); }

color_t gfx_get_pixel(int x, int y) {
  if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return 0;
  return s_fb[y * SCREEN_W + x];
}

void gfx_hline(int x, int y, int w, color_t c) { gfx_fill_rect(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, color_t c) { gfx_fill_rect(x, y, 1, h, c); }

void gfx_line(int x0, int y0, int x1, int y1, color_t c) {
  if (y0 == y1) { gfx_hline(x0 < x1 ? x0 : x1, y0, abs(x1 - x0) + 1, c); return; }
  if (x0 == x1) { gfx_vline(x0, y0 < y1 ? y0 : y1, abs(y1 - y0) + 1, c); return; }
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    put(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void gfx_rect(int x, int y, int w, int h, color_t c) {
  if (w <= 0 || h <= 0) return;
  gfx_hline(x, y, w, c);
  gfx_hline(x, y + h - 1, w, c);
  gfx_vline(x, y + 1, h - 2, c);
  gfx_vline(x + w - 1, y + 1, h - 2, c);
}

// Midpoint circle: calls back with (dx, dy) for every octant point, dx >= dy.
template <typename F> static void circlePoints(int r, F f) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    f(x, y);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

void gfx_circle(int cx, int cy, int r, color_t c) {
  if (r < 0) return;
  circlePoints(r, [&](int x, int y) {
    put(cx + x, cy + y, c); put(cx - x, cy + y, c); put(cx + x, cy - y, c); put(cx - x, cy - y, c);
    put(cx + y, cy + x, c); put(cx - y, cy + x, c); put(cx + y, cy - x, c); put(cx - y, cy - x, c);
  });
}

void gfx_fill_circle(int cx, int cy, int r, color_t c) {
  if (r < 0) return;
  circlePoints(r, [&](int x, int y) {
    gfx_hline(cx - x, cy + y, 2 * x + 1, c); gfx_hline(cx - x, cy - y, 2 * x + 1, c);
    gfx_hline(cx - y, cy + x, 2 * y + 1, c); gfx_hline(cx - y, cy - x, 2 * y + 1, c);
  });
}

// Quarter circles at the four corners of (x, y, w, h) inset by r.
void gfx_round_rect(int x, int y, int w, int h, int r, color_t c) {
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r <= 0) { gfx_rect(x, y, w, h, c); return; }
  gfx_hline(x + r, y, w - 2 * r, c);
  gfx_hline(x + r, y + h - 1, w - 2 * r, c);
  gfx_vline(x, y + r, h - 2 * r, c);
  gfx_vline(x + w - 1, y + r, h - 2 * r, c);
  const int l = x + r, t = y + r, rr = x + w - 1 - r, b = y + h - 1 - r;
  circlePoints(r, [&](int dx, int dy) {
    put(rr + dx, b + dy, c); put(rr + dy, b + dx, c);
    put(l - dx, b + dy, c);  put(l - dy, b + dx, c);
    put(rr + dx, t - dy, c); put(rr + dy, t - dx, c);
    put(l - dx, t - dy, c);  put(l - dy, t - dx, c);
  });
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, color_t c) {
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r <= 0) { gfx_fill_rect(x, y, w, h, c); return; }
  gfx_fill_rect(x, y + r, w, h - 2 * r, c);
  const int l = x + r, t = y + r, b = y + h - 1 - r, span = w - 2 * r;
  circlePoints(r, [&](int dx, int dy) {
    gfx_hline(l - dx, t - dy, span + 2 * dx, c); gfx_hline(l - dx, b + dy, span + 2 * dx, c);
    gfx_hline(l - dy, t - dx, span + 2 * dy, c); gfx_hline(l - dy, b + dx, span + 2 * dy, c);
  });
}

void gfx_triangle(int x0, int y0, int x1, int y1, int x2, int y2, color_t c) {
  gfx_line(x0, y0, x1, y1, c);
  gfx_line(x1, y1, x2, y2, c);
  gfx_line(x2, y2, x0, y0, c);
}

// Scanline fill: sort by y, then walk the long edge (0-2) against the two short ones.
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, color_t c) {
  int t;
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y0 == y2) {   // flat: one row
    int a = x0, b = x0;
    if (x1 < a) a = x1; if (x1 > b) b = x1;
    if (x2 < a) a = x2; if (x2 > b) b = x2;
    gfx_hline(a, y0, b - a + 1, c);
    return;
  }
  for (int y = y0; y <= y2; y++) {
    int a = x0 + (int)((int64_t)(x2 - x0) * (y - y0) / (y2 - y0));
    int b;
    if (y < y1 || y1 == y2) b = y1 == y0 ? x1 : x0 + (int)((int64_t)(x1 - x0) * (y - y0) / (y1 - y0));
    else b = x1 + (int)((int64_t)(x2 - x1) * (y - y1) / (y2 - y1));
    if (a > b) { t = a; a = b; b = t; }
    gfx_hline(a, y, b - a + 1, c);
  }
}

void gfx_blit(int x, int y, int w, int h, const color_t *px) {
  for (int j = 0; j < h; j++) {
    int yy = y + j;
    if (yy < s_cy0 || yy >= s_cy1) continue;
    for (int i = 0; i < w; i++) {
      int xx = x + i;
      if (xx >= s_cx0 && xx < s_cx1) s_fb[yy * SCREEN_W + xx] = px[j * w + i];
    }
  }
}

void gfx_scroll(int dy, color_t fill) {
  if (dy == 0) return;
  if (dy >= SCREEN_H || dy <= -SCREEN_H) { gfx_clear(fill); return; }
  const size_t row = SCREEN_W * sizeof(color_t);
  if (dy > 0) {
    memmove(s_fb, s_fb + dy * SCREEN_W, (SCREEN_H - dy) * row);
    for (int i = (SCREEN_H - dy) * SCREEN_W; i < SCREEN_W * SCREEN_H; i++) s_fb[i] = fill;
  } else {
    dy = -dy;
    memmove(s_fb + dy * SCREEN_W, s_fb, (SCREEN_H - dy) * row);
    for (int i = 0; i < dy * SCREEN_W; i++) s_fb[i] = fill;
  }
}

void gfx_font(int font) {
  if (font >= 0 && font < (int)(sizeof kFonts / sizeof kFonts[0])) s_font = font;
}
int gfx_font_w(void) { return kFonts[s_font].w; }
int gfx_font_h(void) { return kFonts[s_font].h; }
int gfx_text_width(const char *s) { return (int)strlen(s) * kFonts[s_font].w; }

int gfx_text_bg(int x, int y, const char *s, color_t fg, int bg) {
  const Font &f = kFonts[s_font];
  for (; *s; s++, x += f.w) {
    uint8_t ch = (uint8_t)*s;
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = f.data + (ch - 0x20) * f.h;
    if (x >= s_cx1 || x + f.w <= s_cx0 || y >= s_cy1 || y + f.h <= s_cy0) continue;
    for (int r = 0; r < f.h; r++) {
      const uint8_t bits = g[r];
      for (int b = 0; b < f.w; b++) {
        const bool on = f.lsbLeft ? (bits >> b) & 1 : (bits << b) & 0x80;
        if (on) put(x + b, y + r, fg);
        else if (bg >= 0) put(x + b, y + r, (color_t)bg);
      }
    }
  }
  return x;
}

int gfx_text(int x, int y, const char *s, color_t fg) { return gfx_text_bg(x, y, s, fg, -1); }

int gfx_printf(int x, int y, color_t fg, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return gfx_text(x, y, buf, fg);
}

void gfx_flush(void) { hal_lcd_push(0, 0, SCREEN_W, SCREEN_H, s_fb, SCREEN_W); }

void gfx_flush_rect(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w > 0 && h > 0) hal_lcd_push(x, y, w, h, s_fb + y * SCREEN_W + x, SCREEN_W);
}

color_t *gfx_framebuffer(void) { return s_fb; }
