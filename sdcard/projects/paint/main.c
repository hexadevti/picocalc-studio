/* paint - cursor driven drawing: every gfx primitive, partial flushes, direct framebuffer access.
 * Arrows move, SPACE draws, 1-8 picks a color, P/L/R/C/T selects pen/line/rect/circle/triangle,
 * F fills shapes on/off, X clears, I inverts via gfx_framebuffer(), ESC quits. */
#include <picocalc.h>
#include <stdio.h>
#include <stdlib.h>

enum Tool { PEN, LINE, RECT, CIRCLE, TRI };
static const char *const tool_names[] = { "pen", "line", "rect", "circle", "tri" };
static const color_t colors[8] = { WHITE, RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, ORANGE };

#define CANVAS_Y 16
static int cx = SCREEN_W / 2, cy = SCREEN_H / 2;
static int ax = -1, ay, bx = -1, by;     /* anchor points for shapes */
static int color_idx;
static enum Tool tool = PEN;
static bool fill;

static void status(void)
{
    gfx_fill_rect(0, 0, SCREEN_W, CANVAS_Y - 1, DARKGRAY);
    gfx_fill_rect(2, 2, 10, 10, colors[color_idx]);
    gfx_printf(16, 3, WHITE, "%-6s %s  %3d,%3d", tool_names[tool], fill ? "fill" : "line", cx, cy);
    gfx_flush_rect(0, 0, SCREEN_W, CANVAS_Y);
}

static void cursor(bool on)
{
    /* XOR cursor straight into the framebuffer */
    color_t *fb = gfx_framebuffer();
    for (int d = -3; d <= 3; d++) {
        if (cx + d >= 0 && cx + d < SCREEN_W) fb[cy * SCREEN_W + cx + d] ^= 0xFFFF;
        if (d && cy + d >= CANVAS_Y && cy + d < SCREEN_H) fb[(cy + d) * SCREEN_W + cx] ^= 0xFFFF;
    }
    (void)on;
    gfx_flush_rect(cx - 3, cy - 3, 7, 7);
}

static void apply(void)
{
    color_t c = colors[color_idx];
    switch (tool) {
    case PEN:
        gfx_pixel(cx, cy, c);
        return;
    case TRI:
        if (ax < 0) { ax = cx; ay = cy; return; }
        if (bx < 0) { bx = cx; by = cy; return; }
        if (fill) gfx_fill_triangle(ax, ay, bx, by, cx, cy, c);
        else gfx_triangle(ax, ay, bx, by, cx, cy, c);
        ax = bx = -1;
        break;
    default:
        if (ax < 0) { ax = cx; ay = cy; return; }
        if (tool == LINE) gfx_line(ax, ay, cx, cy, c);
        else if (tool == RECT) {
            int x = ax < cx ? ax : cx, y = ay < cy ? ay : cy, w = abs(cx - ax) + 1, h = abs(cy - ay) + 1;
            if (fill) gfx_fill_rect(x, y, w, h, c);
            else gfx_rect(x, y, w, h, c);
        } else {
            int dx = cx - ax, dy = cy - ay, r = 0;
            while (r * r < dx * dx + dy * dy) r++;
            if (fill) gfx_fill_circle(ax, ay, r, c);
            else gfx_circle(ax, ay, r, c);
        }
        ax = -1;
    }
}

void setup(void)
{
    gfx_clear(BLACK);
    gfx_clip(0, CANVAS_Y, SCREEN_W, SCREEN_H - CANVAS_Y);
    gfx_noclip();
    gfx_flush();
    status();
    cursor(true);
}

void loop(void)
{
    int k = kbd_wait();
    int step = (kbd_mods() & MOD_SHIFT) ? 8 : 1;
    cursor(false);
    switch (k) {
    case KEY_ESC: sys_exit();
    case KEY_LEFT: cx -= step; break;
    case KEY_RIGHT: cx += step; break;
    case KEY_UP: cy -= step; break;
    case KEY_DOWN: cy += step; break;
    case ' ': apply(); break;
    case 'p': tool = PEN; ax = bx = -1; break;
    case 'l': tool = LINE; ax = bx = -1; break;
    case 'r': tool = RECT; ax = bx = -1; break;
    case 'c': tool = CIRCLE; ax = bx = -1; break;
    case 't': tool = TRI; ax = bx = -1; break;
    case 'f': fill = !fill; break;
    case 'x': gfx_fill_rect(0, CANVAS_Y, SCREEN_W, SCREEN_H - CANVAS_Y, BLACK); break;
    case 'i': {
        color_t *fb = gfx_framebuffer();
        for (int i = CANVAS_Y * SCREEN_W; i < SCREEN_W * SCREEN_H; i++) fb[i] = (color_t)~fb[i];
        break;
    }
    default:
        if (k >= '1' && k <= '8') color_idx = k - '1';
    }
    if (cx < 0) cx = 0;
    if (cx >= SCREEN_W) cx = SCREEN_W - 1;
    if (cy < CANVAS_Y) cy = CANVAS_Y;
    if (cy >= SCREEN_H) cy = SCREEN_H - 1;
    gfx_flush();
    status();
    cursor(true);
}
