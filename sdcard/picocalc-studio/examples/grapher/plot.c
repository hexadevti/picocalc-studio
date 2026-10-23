/* plot.c - grid with "nice" 1-2-5 tick steps, curves sampled once per pixel column with asymptote
 * detection (a jump bigger than the plot height breaks the line instead of drawing a wall) */
#include "plot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int plot_sx(const View *v, float x) { return v->sx + (int)((x - v->x0) / (v->x1 - v->x0) * (float)v->sw); }
int plot_sy(const View *v, float y) { return v->sy + v->sh - 1 - (int)((y - v->y0) / (v->y1 - v->y0) * (float)v->sh); }
float plot_wx(const View *v, int sx) { return v->x0 + ((float)(sx - v->sx) + 0.5f) * (v->x1 - v->x0) / (float)v->sw; }

/* largest of 1, 2, 5 x 10^k that gives at most max_ticks steps over range */
static float nice_step(float range, int max_ticks)
{
    float raw = range / (float)max_ticks;
    float mag = powf(10.0f, floorf(logf(raw) * 0.434294482f));
    float n = raw / mag;
    return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * mag;
}

static void label(char *buf, int size, float v, float step)
{
    if (fabsf(v) < step * 0.001f) v = 0;
    if (step >= 1) snprintf(buf, (size_t)size, "%d", (int)roundf(v));
    else if (step >= 0.1f) snprintf(buf, (size_t)size, "%.1f", (double)v);
    else snprintf(buf, (size_t)size, "%.2g", (double)v);
}

void plot_grid(const View *v)
{
    gfx_fill_rect(v->sx, v->sy, v->sw, v->sh, RGB(12, 14, 22));
    gfx_clip(v->sx, v->sy, v->sw, v->sh);
    float xs = nice_step(v->x1 - v->x0, 8), ys = nice_step(v->y1 - v->y0, 7);
    int ax = plot_sx(v, 0), ay = plot_sy(v, 0);
    /* labels sit next to the axes, or along the edge when an axis is off screen */
    int lx = ax < v->sx ? v->sx + 2 : ax > v->sx + v->sw - 30 ? v->sx + v->sw - 30 : ax + 3;
    int ly = ay < v->sy ? v->sy + 2 : ay > v->sy + v->sh - 12 ? v->sy + v->sh - 12 : ay + 3;
    char buf[16];
    for (float x = ceilf(v->x0 / xs) * xs; x <= v->x1; x += xs) {
        int sx = plot_sx(v, x);
        gfx_vline(sx, v->sy, v->sh, RGB(32, 36, 52));
        if (fabsf(x) > xs * 0.001f) {
            label(buf, sizeof buf, x, xs);
            gfx_text(sx + 2, ly, buf, GRAY);
        }
    }
    for (float y = ceilf(v->y0 / ys) * ys; y <= v->y1; y += ys) {
        int sy = plot_sy(v, y);
        gfx_hline(v->sx, sy, v->sw, RGB(32, 36, 52));
        if (fabsf(y) > ys * 0.001f) {
            label(buf, sizeof buf, y, ys);
            gfx_text(lx, sy - 10, buf, GRAY);
        }
    }
    gfx_vline(ax, v->sy, v->sh, LIGHTGRAY);
    gfx_hline(v->sx, ay, v->sw, LIGHTGRAY);
    gfx_noclip();
}

int plot_curve(const View *v, const Node *n, float t, color_t c, int style)
{
    if (!n) return 0;
    gfx_clip(v->sx, v->sy, v->sw, v->sh);
    int prev_y = 0, count = 0;
    bool have_prev = false;
    for (int sx = v->sx; sx < v->sx + v->sw; sx++) {
        float y = expr_eval(n, plot_wx(v, sx), t);
        if (y != y || fabsf(y) > 1e7f) { have_prev = false; continue; }   /* NaN or huge */
        int sy = plot_sy(v, y);
        if (sy < -10000) sy = -10000;
        if (sy > 10000) sy = 10000;
        if (style == 1) {
            if ((sx & 3) == 0) gfx_pixel(sx, sy, c);
        } else if (have_prev && abs(sy - prev_y) < v->sh) {
            gfx_line(sx - 1, prev_y, sx, sy, c);
            gfx_line(sx - 1, prev_y + 1, sx, sy + 1, c);   /* 2 px thick */
        } else {
            gfx_pixel(sx, sy, c);
        }
        prev_y = sy;
        have_prev = true;
        count++;
    }
    gfx_noclip();
    return count;
}

void plot_zoom(View *v, float f)
{
    float cx = (v->x0 + v->x1) / 2, cy = (v->y0 + v->y1) / 2;
    float hw = (v->x1 - v->x0) / 2 * f, hh = (v->y1 - v->y0) / 2 * f;
    if (hw < 1e-3f || hw > 1e5f) return;
    v->x0 = cx - hw; v->x1 = cx + hw;
    v->y0 = cy - hh; v->y1 = cy + hh;
}

void plot_pan(View *v, float fx, float fy)
{
    float dx = (v->x1 - v->x0) * fx, dy = (v->y1 - v->y0) * fy;
    v->x0 += dx; v->x1 += dx;
    v->y0 += dy; v->y1 += dy;
}
