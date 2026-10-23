/* grapher - a function plotter for a calculator that is also a computer.
 * Up to four functions of x (and the time t, which animates them). Each is parsed to a syntax
 * tree; its derivative is computed symbolically from that tree (expr.c), which the trace mode
 * uses for exact slopes, tangent lines and Newton's method (roots and extrema).
 * Files: expr.c parser/evaluator/derivative, plot.c grid and curves, main.c UI.
 *
 * Graph:  arrows pan (trace: move / switch)  +/- zoom  TAB select  ENTER edit  t trace
 *         d derivative  r root  m min/max  0 reset view  ESC quit
 * Edit:   type, LEFT/RIGHT/HOME/END, BACKSPACE/DEL, ENTER apply, ESC cancel
 * Syntax: + - * / ^ ( ), 2x and 3sin(x) multiply, pi e x t,
 *         sin cos tan asin acos atan sinh cosh sqrt abs ln log exp floor */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "expr.h"
#include "plot.h"

#define NF 4
#define SRC_MAX 56
#define PANEL_Y 236

typedef struct {
    char src[SRC_MAX];
    Node *f, *d, *d2;        /* function, first and second derivative */
    bool show_d;
} Fn;

static const color_t colors[NF] = { RGB(255, 90, 90), RGB(90, 200, 255), RGB(120, 255, 120), RGB(255, 200, 60) };
static const char *const presets[NF] = { "sin(x)", "x^3/8 - x", "e^(-x^2/4)cos(3x - 2t)", "" };

static Fn fns[NF];
static View view = { -8.0f, 8.0f, -6.0f, 6.0f, 0, 0, SCREEN_W, PANEL_Y - 2 };
static int sel;
static bool editing, tracing, dirty = true;
static char edit_buf[SRC_MAX];
static int edit_cur;
static float trace_x;
static char status[80];
static color_t status_col = GRAY;
static int err_col = -1;
static uint32_t t0;

static void set_status(color_t c, const char *fmt, float a, float b)
{
    snprintf(status, sizeof status, fmt, (double)a, (double)b);
    status_col = c;
}

static void clear_fn(Fn *fn)
{
    expr_free(fn->f);
    expr_free(fn->d);
    expr_free(fn->d2);
    fn->f = fn->d = fn->d2 = NULL;
}

/* parse src into slot i; on error keep the old function and report where */
static bool apply_src(int i, const char *src)
{
    Fn *fn = &fns[i];
    const char *blank = src;
    while (*blank == ' ') blank++;
    if (!*blank) {
        clear_fn(fn);
        fn->src[0] = 0;
        return true;
    }
    const char *msg;
    int pos;
    Node *n = expr_parse(src, &msg, &pos);
    if (!n) {
        snprintf(status, sizeof status, "error: %s", msg);
        status_col = RED;
        err_col = pos;
        return false;
    }
    clear_fn(fn);
    fn->f = n;
    fn->d = expr_deriv(n);
    fn->d2 = expr_deriv(fn->d);
    snprintf(fn->src, sizeof fn->src, "%s", src);
    err_col = -1;
    snprintf(status, sizeof status, "y%d: %d nodes, y%d': %d nodes", i + 1, expr_count(n), i + 1, expr_count(fn->d));
    status_col = GRAY;
    return true;
}

static float now_t(void) { return (float)(sys_millis() - t0) / 1000.0f; }

static bool animated(void)
{
    for (int i = 0; i < NF; i++)
        if (expr_uses_t(fns[i].f)) return true;
    return false;
}

/* Newton's method on g with derivative dg, from x; false if it does not converge */
static bool newton(const Node *g, const Node *dg, float *x, float t)
{
    float v = *x;
    for (int i = 0; i < 40; i++) {
        float y = expr_eval(g, v, t), s = expr_eval(dg, v, t);
        if (y != y || s != s || s == 0) return false;
        float nx = v - y / s;
        if (fabsf(nx - v) < 1e-6f * (1 + fabsf(v))) { *x = nx; return fabsf(expr_eval(g, nx, t)) < 1e-3f; }
        v = nx;
    }
    return false;
}

/* --------------------------------------------------------------------- draw */

static void draw_trace(float t)
{
    Fn *fn = &fns[sel];
    if (!fn->f) return;
    float y = expr_eval(fn->f, trace_x, t), s = expr_eval(fn->d, trace_x, t);
    int sx = plot_sx(&view, trace_x);
    gfx_clip(view.sx, view.sy, view.sw, view.sh);
    gfx_vline(sx, view.sy, view.sh, DARKGRAY);
    if (y == y) {
        int sy = plot_sy(&view, y);
        gfx_hline(view.sx, sy, view.sw, DARKGRAY);
        if (s == s) {   /* tangent line across the view */
            float xa = view.x0, xb = view.x1;
            gfx_line(plot_sx(&view, xa), plot_sy(&view, y + s * (xa - trace_x)),
                     plot_sx(&view, xb), plot_sy(&view, y + s * (xb - trace_x)), WHITE);
        }
        gfx_fill_circle(sx, sy, 4, colors[sel]);
        gfx_circle(sx, sy, 4, WHITE);
    }
    gfx_noclip();
    snprintf(status, sizeof status, "x=%.4g  y=%.4g  dy/dx=%.4g", (double)trace_x, (double)y, (double)s);
    status_col = colors[sel];
}

static void draw_panel(void)
{
    gfx_fill_rect(0, PANEL_Y - 2, SCREEN_W, SCREEN_H - PANEL_Y + 2, RGB(24, 24, 32));
    gfx_hline(0, PANEL_Y - 2, SCREEN_W, GRAY);
    for (int i = 0; i < NF; i++) {
        int y = PANEL_Y + 2 + i * 15;
        if (i == sel) gfx_fill_rect(0, y - 2, SCREEN_W, 14, editing ? RGB(30, 40, 70) : RGB(40, 40, 52));
        gfx_fill_rect(4, y + 1, 8, 8, colors[i]);
        gfx_printf(16, y, WHITE, "y%d%s=", i + 1, fns[i].show_d ? "'" : "");
        const char *s = i == sel && editing ? edit_buf : fns[i].src;
        gfx_text(40, y, *s || (i == sel && editing) ? s : "(empty)", *s ? LIGHTGRAY : DARKGRAY);
        if (i == sel && editing) {
            int cx = 40 + edit_cur * gfx_font_w();
            gfx_vline(cx, y - 1, 11, YELLOW);
            if (err_col >= 0) gfx_hline(40 + err_col * gfx_font_w(), y + 10, gfx_font_w(), RED);
        }
    }
    gfx_text(4, SCREEN_H - 22, status, status_col);
    gfx_text(4, SCREEN_H - 11,
             editing ? "ENTER apply  ESC cancel" : tracing ? "LEFT/RIGHT move  UP/DOWN fn  r root  m extremum"
                                                           : "TAB fn ENTER edit t trace d deriv 0 reset",
             DARKGRAY);
}

static void draw(void)
{
    float t = now_t();
    plot_grid(&view);
    for (int i = 0; i < NF; i++) {
        if (!fns[i].f) continue;
        if (fns[i].show_d) plot_curve(&view, fns[i].d, t, colors[i], 1);
        plot_curve(&view, fns[i].f, t, colors[i], 0);
    }
    if (tracing && !editing) draw_trace(t);
    else if (!editing && fns[sel].show_d && fns[sel].d) {
        char buf[64];
        snprintf(status, sizeof status, "y%d'=%s", sel + 1, expr_print(fns[sel].d, buf, sizeof buf));
        status_col = colors[sel];
    }
    draw_panel();
    gfx_flush();
}

/* ------------------------------------------------------------------- input */

static void edit_key(int k)
{
    int len = (int)strlen(edit_buf);
    if (k == KEY_ESC) { editing = false; err_col = -1; status[0] = 0; return; }
    if (k == KEY_ENTER) { if (apply_src(sel, edit_buf)) editing = false; return; }
    if (k == KEY_LEFT && edit_cur > 0) edit_cur--;
    else if (k == KEY_RIGHT && edit_cur < len) edit_cur++;
    else if (k == KEY_HOME) edit_cur = 0;
    else if (k == KEY_END) edit_cur = len;
    else if (k == KEY_BACKSPACE && edit_cur > 0) {
        memmove(edit_buf + edit_cur - 1, edit_buf + edit_cur, (size_t)(len - edit_cur + 1));
        edit_cur--;
    } else if (k == KEY_DEL && edit_cur < len) {
        memmove(edit_buf + edit_cur, edit_buf + edit_cur + 1, (size_t)(len - edit_cur));
    } else if (k >= 32 && k < 127 && len < SRC_MAX - 1) {
        memmove(edit_buf + edit_cur + 1, edit_buf + edit_cur, (size_t)(len - edit_cur + 1));
        edit_buf[edit_cur++] = (char)k;
    }
    err_col = -1;
}

static void select_next(int dir)
{
    for (int i = 1; i <= NF; i++) {
        int j = (sel + dir * i + NF * 2) % NF;
        if (fns[j].f || !tracing) { sel = j; return; }
    }
}

static void graph_key(int k)
{
    float step = (view.x1 - view.x0) / (float)view.sw;
    switch (k) {
    case KEY_ESC:
        for (int i = 0; i < NF; i++) clear_fn(&fns[i]);
        sys_exit();
        break;
    case KEY_ENTER:
        editing = true;
        strcpy(edit_buf, fns[sel].src);
        edit_cur = (int)strlen(edit_buf);
        status[0] = 0;
        break;
    case KEY_TAB: select_next(1); status[0] = 0; break;
    case '+': case '=': plot_zoom(&view, 0.8f); break;
    case '-': plot_zoom(&view, 1.25f); break;
    case '0': view.x0 = -8; view.x1 = 8; view.y0 = -6; view.y1 = 6; break;
    case 'd': fns[sel].show_d = !fns[sel].show_d; status[0] = 0; break;
    case 't':
        tracing = !tracing;
        trace_x = (view.x0 + view.x1) / 2;
        if (!tracing) status[0] = 0;
        break;
    case 'r': case 'm': {
        Fn *fn = &fns[sel];
        if (!fn->f) break;
        tracing = true;
        float x = trace_x;
        bool ok = k == 'r' ? newton(fn->f, fn->d, &x, now_t()) : newton(fn->d, fn->d2, &x, now_t());
        if (ok) trace_x = x;
        else { set_status(RED, k == 'r' ? "no root found near x=%.3g" : "no extremum near x=%.3g", trace_x, 0); draw(); sys_delay(900); }
        break;
    }
    case KEY_LEFT: if (tracing) trace_x -= step * 2; else plot_pan(&view, -0.1f, 0); break;
    case KEY_RIGHT: if (tracing) trace_x += step * 2; else plot_pan(&view, 0.1f, 0); break;
    case KEY_UP: if (tracing) select_next(-1); else plot_pan(&view, 0, 0.1f); break;
    case KEY_DOWN: if (tracing) select_next(1); else plot_pan(&view, 0, -0.1f); break;
    default: return;
    }
    /* keep the trace cursor on screen by scrolling the view */
    if (tracing && (trace_x < view.x0 || trace_x > view.x1)) {
        float w = view.x1 - view.x0;
        view.x0 = trace_x - w / 2;
        view.x1 = trace_x + w / 2;
    }
}

void setup(void)
{
    for (int i = 0; i < NF; i++) apply_src(i, presets[i]);
    status[0] = 0;
    t0 = sys_millis();
}

void loop(void)
{
    int k = kbd_get();
    if (k) {
        if (editing) edit_key(k);
        else graph_key(k);
        dirty = true;
    }
    if (dirty || animated()) {
        draw();
        dirty = false;
    }
    sys_delay(animated() ? 1 : 10);
}
