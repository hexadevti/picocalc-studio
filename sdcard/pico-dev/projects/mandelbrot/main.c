/* mandelbrot - float vs double performance benchmark and visual check.
 * Renders the same view with float (hardware FPU) and double (soft-float via libgcc), prints the
 * render times and an image checksum. Compare with the gcc reference numbers in tests/ref.
 * Arrows pan, +/- zoom, D toggles float/double, ESC quits. */
#include <picocalc.h>
#include <stdio.h>

#define MAX_ITER 64

static double view_x = -0.6, view_y = 0.0, view_w = 3.0;
static bool use_double;

static color_t shade(int it)
{
    if (it >= MAX_ITER) return BLACK;
    int t = it * 255 / MAX_ITER;
    return RGB(t * 4 & 255, t * 2 & 255, 255 - t);
}

static uint32_t render_float(void)
{
    color_t *fb = gfx_framebuffer();
    float scale = (float)(view_w / SCREEN_W);
    float x0 = (float)(view_x - view_w / 2), y0 = (float)(view_y - view_w / 2);
    uint32_t sum = 0;
    for (int py = 0; py < SCREEN_H; py++) {
        float ci = y0 + (float)py * scale;
        for (int px = 0; px < SCREEN_W; px++) {
            float cr = x0 + (float)px * scale, zr = 0, zi = 0;
            int it = 0;
            while (it < MAX_ITER && zr * zr + zi * zi < 4.0f) {
                float t = zr * zr - zi * zi + cr;
                zi = 2.0f * zr * zi + ci;
                zr = t;
                it++;
            }
            fb[py * SCREEN_W + px] = shade(it);
            sum = sum * 31u + (uint32_t)it;
        }
        if ((py & 15) == 15) sys_yield();
    }
    return sum;
}

static uint32_t render_double(void)
{
    color_t *fb = gfx_framebuffer();
    double scale = view_w / SCREEN_W;
    double x0 = view_x - view_w / 2, y0 = view_y - view_w / 2;
    uint32_t sum = 0;
    for (int py = 0; py < SCREEN_H; py++) {
        double ci = y0 + py * scale;
        for (int px = 0; px < SCREEN_W; px++) {
            double cr = x0 + px * scale, zr = 0, zi = 0;
            int it = 0;
            while (it < MAX_ITER && zr * zr + zi * zi < 4.0) {
                double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                it++;
            }
            fb[py * SCREEN_W + px] = shade(it);
            sum = sum * 31u + (uint32_t)it;
        }
        if ((py & 15) == 15) sys_yield();
    }
    return sum;
}

static void render(void)
{
    uint64_t t0 = sys_micros();
    uint32_t sum = use_double ? render_double() : render_float();
    uint32_t ms = (uint32_t)((sys_micros() - t0) / 1000);
    printf("mandelbrot %s %lu ms checksum %08lx\n", use_double ? "double" : "float", (unsigned long)ms,
           (unsigned long)sum);
    gfx_fill_rect(0, 0, SCREEN_W, 12, BLACK);
    gfx_printf(2, 1, WHITE, "%s %lu ms  zoom %.4g", use_double ? "double" : "float", (unsigned long)ms, 3.0 / view_w);
    gfx_flush();
}

void setup(void)
{
    render();          /* float */
    use_double = true;
    render();          /* double, same view: checksums should match closely */
}

void loop(void)
{
    int k = kbd_wait();
    double step = view_w / 8;
    switch (k) {
    case KEY_ESC: sys_exit();
    case KEY_LEFT: view_x -= step; break;
    case KEY_RIGHT: view_x += step; break;
    case KEY_UP: view_y -= step; break;
    case KEY_DOWN: view_y += step; break;
    case '+': case '=': view_w /= 2; break;
    case '-': view_w *= 2; break;
    case 'd': use_double = !use_double; break;
    default: return;
    }
    render();
}
