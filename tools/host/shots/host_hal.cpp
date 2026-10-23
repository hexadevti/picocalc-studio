// host_hal.cpp - hal.h on the PC for the screenshot harness: an in-memory 320x320 LCD, keys from a
// script, files under $PICOCALC_SD (shim.h), wall-clock time, the LCD's speed. No sound.
//
// $SHOT_SCRIPT is a text file, one action per line (# starts a comment):
//   key NAME [n]     press and release a key n times: ESC ENTER TAB BKSP DEL SPACE UP DOWN LEFT
//                    RIGHT HOME END PGUP PGDN F1..F10, or a single character
//   ctrl X / alt X   the key X with Ctrl / Alt held
//   hold X / release X  press X and keep it down (for kbd_down), release it
//   type TEXT        types the rest of the line
//   wait MS          lets the program run for MS milliseconds
//   shot NAME        writes the LCD to $SHOT_DIR/NAME.ppm
// A key is only sent once the program is idle again (it slept or pushed pixels to the LCD since
// the previous one), so each key sees the screen the last one left. The script's end exits.
#include <chrono>
#include <deque>
#include <string>
#include <thread>
#include <vector>
#include <stdarg.h>
#include <direct.h>
#include "../../../firmware/picocalc_studio/src/hal/hal.h"
#include "../../../firmware/picocalc_studio/src/sdk/picocalc.h"

static const char *sdRoot() {
  const char *r = getenv("PICOCALC_SD");
  return r ? r : ".";
}

extern "C" const char *host_path(const char *path, char *buf, size_t n) {
  if (strncmp(path, "/sd", 3) || (path[3] && path[3] != '/')) return path;
  snprintf(buf, n, "%s%s", sdRoot(), path + 3);
  return buf;
}

#undef fopen
#undef opendir
#undef stat
#undef mkdir
#undef remove
#undef rename
extern "C" FILE *host_fopen(const char *p, const char *m) { char b[512]; return fopen(host_path(p, b, sizeof b), m); }
extern "C" DIR *host_opendir(const char *p) { char b[512]; return opendir(host_path(p, b, sizeof b)); }
extern "C" int host_stat(const char *p, struct stat *st) { char b[512]; return stat(host_path(p, b, sizeof b), st); }
extern "C" int host_mkdir(const char *p) { char b[512]; return _mkdir(host_path(p, b, sizeof b)); }
extern "C" int host_remove(const char *p) { char b[512]; return remove(host_path(p, b, sizeof b)); }
extern "C" int host_rename(const char *a, const char *b) {
  char x[512], y[512];
  return rename(host_path(a, x, sizeof x), host_path(b, y, sizeof y));
}

// ---- time ----

static const auto s_t0 = std::chrono::steady_clock::now();
uint64_t hal_micros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s_t0).count();
}
uint32_t hal_millis() { return (uint32_t)(hal_micros() / 1000); }

static bool s_idle;
void hal_delay(uint32_t ms) {
  s_idle = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---- LCD ----

static uint16_t s_lcd[SCREEN_W * SCREEN_H];

// The device's LCD takes ~16 ms for a full frame over SPI: take as long, so the frame rates the
// programs show are near the device's rather than the PC's.
static void lcdTime(int w, int h) {
  const uint64_t end = hal_micros() + (uint64_t)w * h * 16000 / (SCREEN_W * SCREEN_H);
  while (hal_micros() < end) {}
}

void hal_lcd_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  lcdTime(w, h);
  for (int r = 0; r < h; r++)
    if (y + r >= 0 && y + r < SCREEN_H)
      for (int c = 0; c < w; c++)
        if (x + c >= 0 && x + c < SCREEN_W) s_lcd[(y + r) * SCREEN_W + x + c] = px[r * stride + c];
  s_idle = true;
}

void hal_lcd_fill(int x, int y, int w, int h, uint16_t color) {
  lcdTime(w, h);
  for (int r = y; r < y + h; r++)
    for (int c = x; c < x + w; c++)
      if (r >= 0 && r < SCREEN_H && c >= 0 && c < SCREEN_W) s_lcd[r * SCREEN_W + c] = color;
  s_idle = true;
}

static void writeShot(const std::string &name) {
  const char *dir = getenv("SHOT_DIR");
  std::string path = std::string(dir ? dir : ".") + "/" + name + ".ppm";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) { fprintf(stderr, "shot: can't write %s\n", path.c_str()); exit(4); }
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    const uint16_t p = s_lcd[i];
    const uint8_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    const uint8_t rgb[3] = {(uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  fprintf(stderr, "shot: %s\n", path.c_str());
}

// ---- the key script ----

struct Step {
  enum { KEYS, WAIT, SHOT } kind;
  std::vector<HalKey> keys;   // KEYS: delivered together
  uint32_t ms;
  std::string name;
};
static std::deque<Step> s_steps;
static std::deque<HalKey> s_now;    // the batch being delivered
static uint32_t s_waitEnd;
static bool s_waiting;

static int keyCode(const std::string &n) {
  static const struct { const char *name; int code; } names[] = {
      {"ESC", KEY_ESC}, {"ENTER", KEY_ENTER}, {"TAB", KEY_TAB}, {"BKSP", KEY_BACKSPACE}, {"DEL", KEY_DEL},
      {"SPACE", KEY_SPACE}, {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
      {"HOME", KEY_HOME}, {"END", KEY_END}, {"PGUP", KEY_PGUP}, {"PGDN", KEY_PGDN}, {"F1", KEY_F1},
      {"F2", KEY_F2}, {"F3", KEY_F3}, {"F4", KEY_F4}, {"F5", KEY_F5}, {"F6", KEY_F6}, {"F7", KEY_F7},
      {"F8", KEY_F8}, {"F9", KEY_F9}, {"F10", KEY_F10}};
  for (auto &k : names)
    if (n == k.name) return k.code;
  if (n.size() == 1) return (unsigned char)n[0];
  fprintf(stderr, "script: unknown key %s\n", n.c_str());
  exit(2);
}

static void addKey(int code, int mod) {
  Step s{Step::KEYS, {}, 0, {}};
  if (mod) s.keys.push_back({(uint8_t)mod, HAL_KEY_PRESSED});
  s.keys.push_back({(uint8_t)code, HAL_KEY_PRESSED});
  if (mod) {   // the program reads the modifiers when it takes the key: release them later
    s_steps.push_back(s);
    s = Step{Step::KEYS, {}, 0, {}};
  }
  s.keys.push_back({(uint8_t)code, HAL_KEY_RELEASED});
  if (mod) s.keys.push_back({(uint8_t)mod, HAL_KEY_RELEASED});
  s_steps.push_back(s);
}

static void loadScript(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "script: can't open %s\n", path); exit(2); }
  char line[512];
  while (fgets(line, sizeof line, f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    const size_t sp = l.find(' ');
    const std::string cmd = l.substr(0, sp), arg = sp == std::string::npos ? "" : l.substr(sp + 1);
    if (cmd.empty() || cmd[0] == '#') continue;
    if (cmd == "type") {
      for (char c : arg) addKey((unsigned char)c, 0);
    } else if (cmd == "key") {
      const size_t sp2 = arg.find(' ');
      const int n = sp2 == std::string::npos ? 1 : atoi(arg.c_str() + sp2 + 1);
      for (int i = 0; i < n; i++) addKey(keyCode(arg.substr(0, sp2)), 0);
    } else if (cmd == "ctrl" || cmd == "alt") {
      addKey(keyCode(arg), cmd == "ctrl" ? KEY_CTRL : KEY_ALT);
    } else if (cmd == "hold" || cmd == "release") {
      s_steps.push_back({Step::KEYS, {{(uint8_t)keyCode(arg), cmd == "hold" ? HAL_KEY_PRESSED : HAL_KEY_RELEASED}}, 0, {}});
    } else if (cmd == "wait") {
      s_steps.push_back({Step::WAIT, {}, (uint32_t)atoi(arg.c_str()), {}});
    } else if (cmd == "shot") {
      s_steps.push_back({Step::SHOT, {}, 0, arg});
    } else {
      fprintf(stderr, "script: unknown action %s\n", cmd.c_str());
      exit(2);
    }
  }
  fclose(f);
}

bool hal_key_poll(HalKey *ev) {
  if (hal_millis() > 120000) { fprintf(stderr, "script: timed out\n"); exit(3); }
  if (s_now.empty() && s_idle) {
    while (!s_steps.empty()) {
      Step &s = s_steps.front();
      if (s.kind == Step::WAIT) {
        if (!s_waiting) { s_waiting = true; s_waitEnd = hal_millis() + s.ms; }
        if ((int32_t)(hal_millis() - s_waitEnd) < 0) return false;
        s_waiting = false;
      } else if (s.kind == Step::SHOT) {
        writeShot(s.name);
      } else {
        s_now.assign(s.keys.begin(), s.keys.end());
        s_steps.pop_front();
        s_idle = false;
        break;
      }
      s_steps.pop_front();
    }
    if (s_steps.empty() && s_now.empty()) exit(0);
  }
  if (s_now.empty()) return false;
  *ev = s_now.front();
  s_now.pop_front();
  return true;
}

// ---- the rest ----

void hal_init() {
  const char *script = getenv("SHOT_SCRIPT");
  if (!script) { fprintf(stderr, "set SHOT_SCRIPT\n"); exit(2); }
  loadScript(script);
}

void hal_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("log: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}
void hal_breadcrumb(const char *, ...) {}
bool hal_crash_report(char *, size_t) { return false; }
int hal_crash_pcs(uint32_t *, int) { return 0; }
void hal_set_serial_cmd_hook(bool (*)(const char *)) {}
void hal_reboot() { exit(0); }
void hal_reboot_to_loader() { exit(0); }
void *hal_alloc_big(size_t n) { return calloc(1, n); }
void *hal_alloc_fast(size_t n) { return calloc(1, n); }
void hal_free(void *p) { free(p); }
size_t hal_free_heap() { return 7u << 20; }
uint32_t hal_random() { return 0x2545F491; }
void hal_key_inject(uint8_t, uint8_t) {}
void hal_set_break_hook(void (*)(void)) {}
int hal_battery_percent() { return 87; }
bool hal_battery_charging() { return false; }
void hal_lcd_backlight(uint8_t) {}
void hal_kbd_backlight(uint8_t) {}
void hal_audio_start(uint32_t) {}
void hal_audio_write(const int16_t *, int) {}
void hal_audio_volume(int) {}
uint32_t hal_audio_queued_us() { return 0; }
bool hal_sd_mounted() { return true; }
const char *hal_sd_root() { return "/sd"; }
