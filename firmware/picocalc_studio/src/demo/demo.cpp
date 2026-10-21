// demo.cpp - the built-in demo that proves the SDK on the device. It uses picocalc.h (plus libc
// and the export table) only, like a user program would. Esc leaves a page, Break returns to
// the menu from anywhere.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "../sdk/picocalc.h"
#include "../sdk/sdk_internal.h"
#include "../rt/exports.h"
#include "../rt/runner.h"
#include "demo.h"

static jmp_buf s_menu;

static void onBreak() { longjmp(s_menu, 1); }

static void title(const char *t) {
  gfx_clear(BLACK);
  gfx_font(FONT_7X13);
  gfx_fill_rect(0, 0, SCREEN_W, 17, NAVY);
  gfx_text(4, 2, t, WHITE);
  gfx_font(FONT_6X10);
}

static void footer(const char *t) {
  gfx_fill_rect(0, SCREEN_H - 12, SCREEN_W, 12, DARKGRAY);
  gfx_text(4, SCREEN_H - 11, t, LIGHTGRAY);
}

static void waitEsc() {
  while (kbd_get() != KEY_ESC) sys_delay(10);
}

// ---- 1. graphics ----

static void pageGraphics() {
  title("Graphics");
  for (int i = 0; i < 16; i++) {
    gfx_fill_rect(4 + i * 19, 22, 18, 18, RGB(i * 16, 255 - i * 16, (i * 37) & 255));
  }
  gfx_rect(4, 46, 90, 60, YELLOW);
  gfx_fill_round_rect(100, 46, 90, 60, 10, PURPLE);
  gfx_round_rect(100, 46, 90, 60, 10, WHITE);
  gfx_fill_circle(250, 76, 30, ORANGE);
  gfx_circle(250, 76, 34, RED);
  gfx_fill_triangle(10, 180, 60, 115, 110, 180, DARKGREEN);
  gfx_triangle(10, 180, 60, 115, 110, 180, GREEN);
  for (int i = 0; i <= 10; i++) gfx_line(120, 115, 120 + i * 18, 180, CYAN);
  gfx_font(FONT_8X8);
  gfx_text(4, 190, "FONT_8X8  The quick brown fox", WHITE);
  gfx_font(FONT_7X13);
  gfx_text(4, 202, "FONT_7X13 The quick brown fox", WHITE);
  gfx_font(FONT_6X10);
  gfx_text(4, 218, "FONT_6X10 The quick brown fox jumps", WHITE);
  gfx_text_bg(4, 230, " text with background ", BLACK, YELLOW);
  gfx_clip(200, 240, 60, 30);
  gfx_fill_circle(230, 255, 40, BLUE);
  gfx_text(200, 250, "clipped text here", WHITE);
  gfx_noclip();
  gfx_rect(199, 239, 62, 32, GRAY);

  uint64_t t0 = sys_micros();
  gfx_flush();
  uint32_t full = (uint32_t)(sys_micros() - t0);
  t0 = sys_micros();
  gfx_flush_rect(0, 280, SCREEN_W, 20);
  uint32_t part = (uint32_t)(sys_micros() - t0);
  gfx_printf(4, 250, GREEN, "flush %lu us", (unsigned long)full);
  gfx_printf(4, 262, GREEN, "20 rows %lu us", (unsigned long)part);
  footer("Space: animation   Esc: back");
  gfx_flush();

  for (;;) {
    int k = kbd_get();
    if (k == KEY_ESC) return;
    if (k != ' ') { sys_delay(10); continue; }
    // Bouncing ball, full-screen flushes: shows the achievable frame rate.
    float x = 160, y = 160, vx = 3.1f, vy = 2.3f;
    uint32_t frames = 0, t = sys_millis(), fps = 0;
    while (kbd_get() != KEY_ESC) {
      gfx_clear(BLACK);
      x += vx; y += vy;
      if (x < 12 || x > SCREEN_W - 12) vx = -vx;
      if (y < 30 || y > SCREEN_H - 12) vy = -vy;
      gfx_fill_circle((int)x, (int)y, 10, RGB((int)x & 255, (int)y & 255, 200));
      gfx_printf(4, 4, WHITE, "%lu fps  Esc: stop", (unsigned long)fps);
      gfx_flush();
      frames++;
      if (sys_millis() - t >= 1000) { fps = frames; frames = 0; t = sys_millis(); }
      sys_yield();
    }
    return;
  }
}

// ---- 2. keyboard ----

static void pageKeys() {
  title("Keyboard events");
  footer("Esc twice: back   (Break: menu)");
  gfx_flush();
  int y = 22, escs = 0;
  for (;;) {
    key_event_t ev;
    if (!kbd_event(&ev)) { sys_delay(5); continue; }
    if (ev.state == KEY_PRESSED) {
      escs = ev.code == KEY_ESC ? escs + 1 : 0;
      if (escs == 2) return;
    }
    if (y > SCREEN_H - 26) {
      gfx_fill_rect(0, 18, SCREEN_W, SCREEN_H - 30, BLACK);
      y = 22;
    }
    const char *st = ev.state == KEY_PRESSED ? "press  " : ev.state == KEY_HELD ? "held   " : "release";
    char ch = ev.code >= 32 && ev.code < 127 ? (char)ev.code : ' ';
    gfx_printf(4, y, ev.state == KEY_PRESSED ? WHITE : GRAY, "%s 0x%02X '%c' mods %c%c%c%c",
               st, ev.code, ch, ev.mods & MOD_SHIFT ? 'S' : '-', ev.mods & MOD_CTRL ? 'C' : '-',
               ev.mods & MOD_ALT ? 'A' : '-', ev.mods & MOD_SYM ? 'Y' : '-');
    gfx_flush_rect(0, y, SCREEN_W, 10);
    y += 10;
  }
}

// ---- 3. sound ----

static void pageSound() {
  static int16_t buf[2205];          // 50 ms at 44.1 kHz
  int vol = 7;
  for (;;) {
    title("Sound");
    gfx_text(4, 24, "1  scale with snd_tone()", WHITE);
    gfx_text(4, 36, "2  sine sweep with snd_play() 44.1 kHz", WHITE);
    gfx_text(4, 48, "3  chord with snd_play() 11.025 kHz", WHITE);
    gfx_text(4, 60, "-/+  volume", WHITE);
    gfx_printf(4, 80, YELLOW, "volume %d", vol);
    footer("Esc: back");
    gfx_flush();
    int k = kbd_wait();
    if (k == KEY_ESC) { snd_stop(); return; }
    if (k == '-' && vol > 0) snd_volume(--vol);
    if ((k == '+' || k == '=') && vol < 10) snd_volume(++vol);
    if (k == '1') {
      static const int notes[] = {262, 294, 330, 349, 392, 440, 494, 523};
      for (int n : notes) {
        snd_tone(n, 180);
        while (snd_busy()) sys_delay(5);
        sys_delay(20);
      }
    }
    if (k == '2') {
      double ph = 0;
      uint32_t t0 = sys_millis();
      for (int c = 0; c < 40; c++) {          // 2 s, 200 Hz -> 2 kHz
        for (int i = 0; i < 2205; i++) {
          double f = 200.0 + 1800.0 * (c * 2205 + i) / (40.0 * 2205);
          ph += 2 * M_PI * f / 44100.0;
          buf[i] = (int16_t)(sin(ph) * 9000);
        }
        snd_play(buf, 2205, 44100);
      }
      while (snd_busy()) sys_delay(5);
      gfx_printf(4, 100, GREEN, "sweep took %lu ms (2000 expected)", (unsigned long)(sys_millis() - t0));
      gfx_flush();
      sys_delay(1200);
    }
    if (k == '3') {
      float p1 = 0, p2 = 0, p3 = 0;
      for (int c = 0; c < 20; c++) {          // 1 s at 11025 Hz, C major
        for (int i = 0; i < 551; i++) {
          p1 += 261.6f / 11025; p2 += 329.6f / 11025; p3 += 392.0f / 11025;
          if (p1 >= 1) p1 -= 1;
          if (p2 >= 1) p2 -= 1;
          if (p3 >= 1) p3 -= 1;
          buf[i] = (int16_t)(3000 * (sinf(p1 * 6.2832f) + sinf(p2 * 6.2832f) + sinf(p3 * 6.2832f)));
        }
        snd_play(buf, 551, 11025);
      }
    }
  }
}

// ---- 4. SD card ----

static void pageSd() {
  title("SD card");
  int y = 24;
  const char *path = "/sd/picocalc_studio_test.txt";
  FILE *f = fopen(path, "w");
  if (!f) {
    gfx_printf(4, y, RED, "fopen(%s) failed", path);
  } else {
    uint32_t now = sys_millis();
    fprintf(f, "PicoCalc Studio SD test\nuptime %lu ms\n", (unsigned long)now);
    fclose(f);
    gfx_printf(4, y, GREEN, "wrote %s", path);
    y += 12;
    char line[64];
    f = fopen(path, "r");
    int n = 0;
    while (f && fgets(line, sizeof line, f)) {
      line[strcspn(line, "\n")] = 0;
      gfx_printf(12, y, WHITE, "read: %s", line);
      y += 10;
      n++;
    }
    if (f) fclose(f);
    gfx_text(4, y, n == 2 ? "read back OK" : "read back FAILED", n == 2 ? GREEN : RED);
  }
  y += 16;
  gfx_text(4, y, "/sd:", YELLOW);
  y += 12;
  DIR *d = opendir("/sd");
  if (!d) {
    gfx_text(12, y, "opendir failed", RED);
  } else {
    struct dirent *e;
    int col = 0;
    while ((e = readdir(d)) && y < SCREEN_H - 24) {
      gfx_printf(12 + col * 152, y, e->d_type == DT_DIR ? CYAN : WHITE, "%.24s%s", e->d_name,
                 e->d_type == DT_DIR ? "/" : "");
      if (++col == 2) { col = 0; y += 10; }
    }
    closedir(d);
  }
  footer("Esc: back");
  gfx_flush();
  waitEsc();
}

// ---- 5. power ----

static void pagePower() {
  int lcd = 255, kb = 128;
  for (;;) {
    title("Power");
    bool chg = false;
    int pct = sys_battery(&chg);
    if (pct < 0) gfx_text(4, 24, "battery: no reading yet", GRAY);
    else gfx_printf(4, 24, WHITE, "battery %d%%%s", pct, chg ? "  (charging)" : "");
    gfx_printf(4, 40, WHITE, "LCD backlight %d   (Up/Down)", lcd);
    gfx_printf(4, 52, WHITE, "keyboard backlight %d   (Left/Right)", kb);
    gfx_printf(4, 72, GRAY, "free heap %u KB", (unsigned)(sys_free_heap() >> 10));
    gfx_printf(4, 84, GRAY, "uptime %lu s", (unsigned long)(sys_millis() / 1000));
    footer("Esc: back");
    gfx_flush();
    int k = KEY_NONE;
    for (uint32_t t = sys_millis(); k == KEY_NONE && sys_millis() - t < 1000; sys_delay(10)) k = kbd_get();
    if (k == KEY_ESC) return;
    if (k == KEY_UP && lcd < 255) lcd = lcd + 32 > 255 ? 255 : lcd + 32;
    if (k == KEY_DOWN && lcd > 16) lcd = lcd - 32 < 16 ? 16 : lcd - 32;
    if (k == KEY_RIGHT && kb < 255) kb = kb + 32 > 255 ? 255 : kb + 32;
    if (k == KEY_LEFT && kb > 0) kb = kb - 32 < 0 ? 0 : kb - 32;
    sys_backlight((uint8_t)lcd);
    kbd_backlight((uint8_t)kb);
  }
}

// ---- 6. export table ----

static void pageExports() {
  title("Export table");
  int y = 24;
  bool sorted = true;
  for (uint32_t i = 1; i < rt_exports_count; i++)
    if (strcmp(rt_exports[i - 1].name, rt_exports[i].name) >= 0) sorted = false;
  gfx_printf(4, y, sorted ? GREEN : RED, "%lu symbols, %s", (unsigned long)rt_exports_count,
             sorted ? "sorted" : "NOT SORTED");
  y += 14;

  // Call through the table the way a compiled program will.
  int pass = 0, total = 0;
  auto check = [&](const char *what, bool ok) {
    gfx_printf(4, y, ok ? GREEN : RED, "%s %s", ok ? "ok  " : "FAIL", what);
    y += 10;
    total++;
    pass += ok;
  };
  auto pStrlen = (size_t (*)(const char *))rt_find_export("strlen");
  check("strlen(\"picocalc\") == 8", pStrlen && pStrlen("picocalc") == 8);
  auto pSnprintf = (int (*)(char *, size_t, const char *, ...))rt_find_export("snprintf");
  char b[32] = "";
  if (pSnprintf) pSnprintf(b, sizeof b, "%d-%s-%.2f", 42, "x", 1.5);
  check("snprintf -> \"42-x-1.50\"", !strcmp(b, "42-x-1.50"));
  auto pSqrt = (double (*)(double))rt_find_export("sqrt");
  check("sqrt(2.0)", pSqrt && fabs(pSqrt(2.0) - 1.41421356) < 1e-6);
  auto pMul = (double (*)(double, double))rt_find_export("__muldf3");
  check("__muldf3(1.5, -4.0) == -6.0", pMul && pMul(1.5, -4.0) == -6.0);
  auto pDiv = (long long (*)(long long, long long))rt_find_export("__divdi3");
  check("__divdi3(-10^12, 7)", pDiv && pDiv(-1000000000000LL, 7) == -142857142857LL);
  auto pMillis = (uint32_t (*)(void))rt_find_export("sys_millis");
  check("sys_millis()", pMillis && pMillis() - sys_millis() < 2);
  auto pFill = (void (*)(int, int, int, int, color_t))rt_find_export("gfx_fill_rect");
  if (pFill) pFill(300, 24, 14, 10, MAGENTA);
  check("gfx_fill_rect (magenta box, top right)", pFill && gfx_get_pixel(305, 28) == MAGENTA);
  check("unknown symbol -> NULL", !rt_find_export("no_such_function"));
  check("first and last entries resolve",
        rt_find_export(rt_exports[0].name) == rt_exports[0].addr &&
        rt_find_export(rt_exports[rt_exports_count - 1].name) == rt_exports[rt_exports_count - 1].addr);
  y += 6;
  gfx_printf(4, y, pass == total ? GREEN : RED, "%d / %d passed", pass, total);
  sys_log("demo exports: %d/%d passed, %lu symbols", pass, total, (unsigned long)rt_exports_count);
  footer("Esc: back");
  gfx_flush();
  waitEsc();
}

// ---- menu ----

void demo_run() {
  const sdk_exit_fn prevExit = sdk_set_exit_handler(onBreak);
  if (setjmp(s_menu)) sdk_reset();   // back here after Break
  for (;;) {
    title("PicoCalc Studio - SDK demo");
    static const char *items[] = {"1  Graphics and fonts", "2  Keyboard events", "3  Sound",
                                  "4  SD card",           "5  Battery and backlight",
                                  "6  Export table check", "L  Back to PicoMultiLoader"};
    for (int i = 0; i < 7; i++) gfx_text(16, 30 + i * 16, items[i], WHITE);
    gfx_printf(16, 160, GRAY, "free heap %u KB", (unsigned)(sys_free_heap() >> 10));
    footer("Break: back to this menu   Esc: back to the IDE");
    gfx_flush();
    kbd_flush();
    // Wait for a key, running any program queued by "@run" from the serial port meanwhile.
    int k = kbd_get();
    bool ran = false;
    while (k == KEY_NONE && !(ran = rt_service())) {
      sys_delay(10);
      k = kbd_get();
    }
    if (ran) continue;
    switch (k) {
      case '1': pageGraphics(); break;
      case '2': pageKeys(); break;
      case '3': pageSound(); break;
      case '4': pageSd(); break;
      case '5': pagePower(); break;
      case '6': pageExports(); break;
      case 'l': case 'L': sys_exit_to_loader();
      case KEY_ESC: sdk_set_exit_handler(prevExit); return;
    }
    snd_stop();
  }
}
