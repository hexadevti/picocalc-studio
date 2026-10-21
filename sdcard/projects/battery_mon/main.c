/* battery_mon - battery and backlight API with periodic redraw.
 * Samples sys_battery() every 2 s into a ring buffer and plots the history; UP/DOWN change the
 * LCD backlight, LEFT/RIGHT the keyboard backlight. Also shows uptime and free heap. ESC quits. */
#include <picocalc.h>
#include <stdio.h>

#define HIST 150
#define SAMPLE_MS 2000

static uint8_t history[HIST];
static int hist_len, hist_pos;
static int lcd_bl = 200, kbd_bl = 64;
static uint32_t next_sample;
static int pct;
static bool charging;

static void sample(void)
{
    pct = sys_battery(&charging);
    history[hist_pos] = (uint8_t)pct;
    hist_pos = (hist_pos + 1) % HIST;
    if (hist_len < HIST) hist_len++;
}

static color_t level_color(int p) { return p > 50 ? GREEN : p > 20 ? YELLOW : RED; }

static void draw(void)
{
    gfx_clear(BLACK);
    gfx_font(FONT_7X13);
    gfx_text(10, 8, "Battery monitor", WHITE);
    gfx_font(FONT_6X10);

    /* battery icon */
    gfx_rect(20, 40, 120, 50, WHITE);
    gfx_fill_rect(140, 55, 6, 20, WHITE);
    gfx_fill_rect(23, 43, 114 * pct / 100, 44, level_color(pct));
    gfx_printf(160, 50, WHITE, "%d%%", pct);
    gfx_text(160, 66, charging ? "charging" : "on battery", charging ? CYAN : LIGHTGRAY);

    /* history graph, oldest on the left */
    int gx = 10, gy = 110, gw = HIST * 2, gh = 100;
    gfx_rect(gx - 1, gy - 1, gw + 2, gh + 2, DARKGRAY);
    for (int p = 25; p < 100; p += 25) gfx_hline(gx, gy + gh - gh * p / 100, gw, DARKGRAY);
    int start = (hist_pos - hist_len + HIST) % HIST;
    for (int i = 1; i < hist_len; i++) {
        int a = history[(start + i - 1) % HIST], b = history[(start + i) % HIST];
        gfx_line(gx + (i - 1) * 2, gy + gh - gh * a / 100, gx + i * 2, gy + gh - gh * b / 100, level_color(b));
    }

    uint32_t s = sys_millis() / 1000;
    gfx_printf(10, 225, LIGHTGRAY, "uptime %02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
               (unsigned long)(s % 60));
    gfx_printf(10, 240, LIGHTGRAY, "free heap %lu", (unsigned long)sys_free_heap());
    gfx_printf(10, 260, WHITE, "LCD backlight  %3d  (UP/DOWN)", lcd_bl);
    gfx_printf(10, 275, WHITE, "kbd backlight  %3d  (LEFT/RIGHT)", kbd_bl);
    gfx_text(10, 300, "ESC quit", GRAY);
    gfx_flush();
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void setup(void)
{
    sys_backlight((uint8_t)lcd_bl);
    kbd_backlight((uint8_t)kbd_bl);
    sample();
    next_sample = sys_millis() + SAMPLE_MS;
    draw();
}

void loop(void)
{
    int k = kbd_get();
    bool redraw = true;
    switch (k) {
    case KEY_ESC: sys_backlight(200); sys_exit();
    case KEY_UP: lcd_bl = clamp255(lcd_bl + 16); sys_backlight((uint8_t)lcd_bl); break;
    case KEY_DOWN: lcd_bl = clamp255(lcd_bl - 16); if (lcd_bl < 16) lcd_bl = 16; sys_backlight((uint8_t)lcd_bl); break;
    case KEY_RIGHT: kbd_bl = clamp255(kbd_bl + 16); kbd_backlight((uint8_t)kbd_bl); break;
    case KEY_LEFT: kbd_bl = clamp255(kbd_bl - 16); kbd_backlight((uint8_t)kbd_bl); break;
    default: redraw = false;
    }
    if ((int32_t)(sys_millis() - next_sample) >= 0) {
        next_sample += SAMPLE_MS;
        sample();
        redraw = true;
    }
    if (redraw) draw();
    sys_delay(20);
}
