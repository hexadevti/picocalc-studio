/* voxel - fly through rings over endless voxel terrain (Comanche-style height-field rendering).
 * A 512x512 wrapping height map is grown by diamond-square at start-up, then lit and coloured
 * (water, sand, grass, forest, rock, snow). Every frame the terrain is ray-marched front to back:
 * one row of map samples per depth step, a y-buffer per screen column so each pixel is written
 * once, distance fog, a banked horizon and a sky with a sun. The rings are occluded by the terrain
 * by snapshotting the y-buffer when the march passes their depth.
 * The bottom strip shows the map, the flight data and where the frame time goes (3D render
 * against LCD transfer) for each quality level.
 *
 *   LEFT/RIGHT bank  UP/DOWN pitch (i inverts)  a/z throttle  q quality  m mute  ENTER start  ESC quit */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_SH 9
#define MAP_N (1 << MAP_SH)
#define MAP_M (MAP_N - 1)
#define VIEW_H 240
#define HUD_Y VIEW_H
#define FOVK 0.75f                    /* half-width of the view plane at distance 1 */
#define SCALE (160.0f / FOVK)         /* pixels per unit at distance 1 */
#define VS 0.5f                       /* one height unit is half a map cell */
#define WATER 46
#define STEP_MS 16
#define DT (STEP_MS / 1000.0f)
#define PI 3.14159265f
#define MAX_RINGS 3
#define PLANE_D 12.0f                 /* the plane is this far ahead of the camera */
#define PLANE_Y 64                    /* and drawn this many pixels below the horizon */
#define PLANE_DROP (PLANE_Y * PLANE_D / (SCALE * VS))   /* so this much lower, in height units */
#define MAX_PARTS 90
#define MINI 76
#define HISCORE_FILE SD_ROOT "/picocalc-studio/voxel.hi"

typedef struct { int cols, colw; float far, dz0, ddz; const char *name; } Quality;

static const Quality qual[] = {
    { 80, 4, 360.0f, 1.0f, 0.020f, "LOW" },
    { 160, 2, 560.0f, 0.6f, 0.010f, "MED" },
    { 320, 1, 760.0f, 0.5f, 0.006f, "HIGH" },
    { 320, 1, 1200.0f, 0.25f, 0.003f, "ULTRA" },
};
#define NQUAL 4

typedef struct { float x, y, alt, dir, r; } Ring;
typedef struct { float x, y, vx, vy; int life; color_t c; } Part;

enum { ST_TITLE, ST_PLAY, ST_OVER };

static uint8_t *hmap;
static color_t *cmap;
static color_t minimap[MINI * MINI];
#define SKY_OFF 800
static color_t sky_tab[2 * SKY_OFF];       /* sky colour by pixels above the horizon, -SKY_OFF.. */
#define sky (sky_tab + SKY_OFF)
typedef struct { int16_t y, h; } Col;     /* y-buffer (top of what is drawn) and horizon per column */
static Col cols[320];
static int16_t snap[MAX_RINGS][320];
static Part parts[MAX_PARTS];

static float px, py, alt, yaw, pitch, roll, speed = 34.0f;
static Ring rings[MAX_RINGS];
static int nrings_done, best, state = ST_TITLE, quality = 1;
static float time_left;
static int crash_t, invuln_t, flash_t, over_t;
static bool inverted, muted, pull_up, new_best;
static uint32_t last_ms, t0, hud_ms;
static float fps, ms_render, ms_lcd;
static float sun_yaw = 0.6f, view_hc = VIEW_H * 0.42f;

/* ============================================================ terrain */

static float frand(float a, float b) { return a + (b - a) * (float)(sys_random() & 0xFFFF) / 65535.0f; }

static int H(int x, int y) { return hmap[((y & MAP_M) << MAP_SH) | (x & MAP_M)]; }

static float ground_at(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    float a = H(ix, iy) + (H(ix + 1, iy) - H(ix, iy)) * fx;
    float b = H(ix, iy + 1) + (H(ix + 1, iy + 1) - H(ix, iy + 1)) * fx;
    return a + (b - a) * fy;
}

static color_t mix565(float r, float g, float b, float k)
{
    r *= k; g *= k; b *= k;
    return RGB(r > 255 ? 255 : (int)r, g > 255 ? 255 : (int)g, b > 255 ? 255 : (int)b);
}

static void gen_terrain(void)
{
    int *t = malloc(MAP_N * MAP_N * sizeof(int));
    #define T(x, y) t[(((y) & MAP_M) << MAP_SH) | ((x) & MAP_M)]
    T(0, 0) = 0;
    int amp = 150 << 8;
    for (int step = MAP_N; step > 1; step >>= 1) {
        int half = step >> 1;
        for (int y = 0; y < MAP_N; y += step)                       /* square step */
            for (int x = 0; x < MAP_N; x += step) {
                int avg = (T(x, y) + T(x + step, y) + T(x, y + step) + T(x + step, y + step)) >> 2;
                T(x + half, y + half) = avg + (int)(sys_random() % (unsigned)(2 * amp + 1)) - amp;
            }
        for (int y = 0; y < MAP_N; y += half)                       /* diamond step */
            for (int x = ((y / half) & 1) ? 0 : half; x < MAP_N; x += step) {
                int avg = (T(x - half, y) + T(x + half, y) + T(x, y - half) + T(x, y + half)) >> 2;
                T(x, y) = avg + (int)(sys_random() % (unsigned)(2 * amp + 1)) - amp;
            }
        amp = amp * 13 / 25;
    }
    int lo = 1 << 30, hi = -(1 << 30);
    for (int i = 0; i < MAP_N * MAP_N; i++) { if (t[i] < lo) lo = t[i]; if (t[i] > hi) hi = t[i]; }
    /* normalise, then a curve that flattens the lowlands and sharpens the peaks */
    uint8_t curve[256];
    for (int i = 0; i < 256; i++) curve[i] = (uint8_t)(255.0f * powf(i / 255.0f, 1.7f));
    for (int i = 0; i < MAP_N * MAP_N; i++) hmap[i] = curve[(int)((int64_t)(t[i] - lo) * 255 / (hi - lo))];
    #undef T
    for (int pass = 0; pass < 2; pass++) {                          /* blur away the grid seams */
        for (int y = 0; y < MAP_N; y++)
            for (int x = 0; x < MAP_N; x++)
                t[(y << MAP_SH) | x] = (4 * H(x, y) + H(x - 1, y) + H(x + 1, y) + H(x, y - 1) + H(x, y + 1) + 4) >> 3;
        for (int i = 0; i < MAP_N * MAP_N; i++) hmap[i] = (uint8_t)t[i];
    }
    free(t);

    for (int y = 0; y < MAP_N; y++)
        for (int x = 0; x < MAP_N; x++) {
            int h = H(x, y), i = (y << MAP_SH) | x;
            float n = (float)(sys_random() % 1000) / 1000.0f;
            float r, g, b;
            if (h < WATER) {
                float d = (WATER - h) / (float)WATER;
                r = 30 - 20 * d; g = 110 - 60 * d + n * 10; b = 170 - 50 * d + n * 12;
                cmap[i] = RGB((int)r, (int)g, (int)b);
                hmap[i] = WATER;
                continue;
            }
            int slope = H(x - 1, y - 1) - H(x + 1, y + 1);              /* light from the north-west */
            float k = 1.0f + slope * 0.045f;
            if (k < 0.45f) k = 0.45f;
            if (k > 1.5f) k = 1.5f;
            int steep = abs(H(x + 1, y) - H(x - 1, y)) + abs(H(x, y + 1) - H(x, y - 1));
            if (h < WATER + 4) { r = 200; g = 185; b = 130; }
            else if (h > 205 - n * 20 && steep < 14) { r = 235; g = 240; b = 250; }
            else if (h > 135 + n * 25 || steep > 16) { r = 120 + n * 30; g = 110 + n * 25; b = 100 + n * 20; }
            else if (h > 80 && n > 0.35f) { r = 30 + n * 20; g = 90 + n * 30; b = 35; }
            else { r = 70 + n * 30; g = 140 + n * 30; b = 50 + n * 10; }
            cmap[i] = mix565(r, g, b, k);
        }
    for (int y = 0; y < MINI; y++)
        for (int x = 0; x < MINI; x++) {
            int i = ((y * MAP_N / MINI) << MAP_SH) | (x * MAP_N / MINI);
            minimap[y * MINI + x] = (cmap[i] >> 1) & 0x7BEF;
        }
}

/* ============================================================ sound */

#define RATE 11025
#define LEAD_SAMPLES (RATE / 8)
#define BLOCK 256
static int16_t audio[BLOCK];
static uint32_t sent;
static float eng_phase, chime_f, chime_t, boom_t, beep_phase, lp;
static int chime_n;

static void synth(int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        float s = 0;
        if (state != ST_OVER && crash_t == 0) {
            eng_phase += (40.0f + speed * 1.3f) / RATE;
            if (eng_phase >= 1) eng_phase -= 1;
            float saw = eng_phase * 2 - 1;
            float noise = (float)(int)(sys_random() & 0xFFF) / 2048.0f - 1;
            s += saw * 0.10f + noise * (0.02f + speed * 0.0006f);
        }
        if (chime_t > 0) {                     /* three rising notes */
            float f = chime_f * (chime_n == 0 ? 1.0f : chime_n == 1 ? 1.26f : 1.5f);
            beep_phase += f / RATE;
            if (beep_phase >= 1) beep_phase -= 1;
            s += (beep_phase < 0.5f ? 0.22f : -0.22f) * chime_t;
            chime_t -= 1.0f / (RATE * 0.12f);
            if (chime_t <= 0 && chime_n < 2) { chime_n++; chime_t = 1; }
        }
        if (boom_t > 0) {
            float noise = (float)(int)(sys_random() & 0xFFF) / 2048.0f - 1;
            lp += (noise - lp) * 0.08f;
            s += lp * 2.2f * boom_t;
            boom_t -= 1.0f / (RATE * 1.4f);
        }
        if (pull_up && ((sent + i) / 1800) % 2 == 0) {
            beep_phase += 1200.0f / RATE;
            if (beep_phase >= 1) beep_phase -= 1;
            s += beep_phase < 0.5f ? 0.08f : -0.08f;
        }
        if (muted) s = 0;
        int v = (int)(s * 16000.0f);
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static void pump_audio(uint32_t ms)
{
    uint32_t target = (uint32_t)((uint64_t)ms * RATE / 1000) + LEAD_SAMPLES;
    if (target > sent + RATE / 2 || target < sent) sent = target - LEAD_SAMPLES;   /* stalled: skip ahead */
    while (sent < target) {
        int n = target - sent > BLOCK ? BLOCK : (int)(target - sent);
        synth(audio, n);
        snd_play(audio, n, RATE);
        sent += (uint32_t)n;
    }
}

/* ============================================================ rings */

static float ring_ground(float x, float y, float r)
{
    float m = 0;
    for (int a = 0; a < 8; a++) {
        float g = ground_at(x + cosf(a * PI / 4) * r, y + sinf(a * PI / 4) * r);
        if (g > m) m = g;
    }
    float g = ground_at(x, y);
    return g > m ? g : m;
}

static void place_ring(Ring *r, const Ring *prev)
{
    float dir = prev->dir + frand(-0.9f, 0.9f);
    float d = frand(150, 210);
    r->x = prev->x + cosf(dir) * d;
    r->y = prev->y + sinf(dir) * d;
    r->dir = dir;
    r->r = 15.0f - nrings_done * 0.25f;
    if (r->r < 8) r->r = 8;
    float g = ring_ground(r->x, r->y, r->r + 4);
    r->alt = g + r->r / VS + 8 + frand(0, 40);
    if (r->alt > 480) r->alt = 480;
}

static void reset_rings(void)
{
    Ring start = { px, py, alt, yaw, 15 };
    place_ring(&rings[0], &start);
    for (int i = 1; i < MAX_RINGS; i++) place_ring(&rings[i], &rings[i - 1]);
}

/* ============================================================ game state */

static void spawn_plane(void)
{
    px = 100; py = 300; yaw = 0;
    for (int i = 0; i < 200 && ground_at(px, py) > WATER + 10; i++) { px += 37; py += 11; }   /* start over a valley */
    alt = ring_ground(px, py, 20) + 50;
    pitch = roll = 0;
    speed = 34;
}

static void new_game(void)
{
    state = ST_PLAY;
    spawn_plane();
    nrings_done = 0;
    time_left = 30;
    new_best = false;
    reset_rings();
    invuln_t = 120;
}

static void game_over(void)
{
    state = ST_OVER;
    over_t = 0;
    if (nrings_done > best) {
        best = nrings_done;
        new_best = true;
        FILE *f = fopen(HISCORE_FILE, "w");
        if (f) { fprintf(f, "%d\n", best); fclose(f); }
    }
}

static void crash(void)
{
    crash_t = 75;
    flash_t = 6;
    boom_t = 1;
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &parts[i];
        float a = frand(0, 2 * PI), v = frand(0.5f, 5.0f);
        p->x = 160; p->y = view_hc + PLANE_Y;
        p->vx = cosf(a) * v; p->vy = sinf(a) * v - 2.5f;
        p->life = 30 + (int)(sys_random() % 50);
        p->c = (sys_random() & 1) ? ORANGE : (sys_random() & 1) ? YELLOW : RGB(255, 80, 20);
    }
    if (state == ST_PLAY) time_left -= 5;
}

static void step(void)
{
    if (state == ST_OVER) over_t++;
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &parts[i];
        if (p->life <= 0) continue;
        p->x += p->vx; p->y += p->vy; p->vy += 0.12f; p->vx *= 0.98f;
        p->life--;
    }
    if (flash_t) flash_t--;
    if (crash_t) {
        if (--crash_t == 0) {
            alt = ring_ground(px, py, 25) + 60;
            pitch = 0.1f; roll = 0;
            invuln_t = 120;
            if (state == ST_PLAY && time_left <= 0) game_over();
        }
        return;
    }
    if (invuln_t) invuln_t--;

    float turn = 0, climb = 0;
    if (state == ST_PLAY) {
        turn = (kbd_down(KEY_RIGHT) ? 1.0f : 0.0f) - (kbd_down(KEY_LEFT) ? 1.0f : 0.0f);
        climb = (kbd_down(KEY_UP) ? 1.0f : 0.0f) - (kbd_down(KEY_DOWN) ? 1.0f : 0.0f);
        if (inverted) climb = -climb;
        if (kbd_down('a')) speed += 25 * DT;
        if (kbd_down('z')) speed -= 25 * DT;
        if (speed < 18) speed = 18;
        if (speed > 75) speed = 75;
    } else {                                    /* attract mode: drift around, keep clear of hills */
        uint32_t t = sys_millis() - t0;
        turn = sinf(t * 0.00031f) * 0.8f;
        float ahead = ring_ground(px + cosf(yaw) * 40, py + sinf(yaw) * 40, 10);
        climb = alt < ahead + 45 ? 0.8f : alt > ahead + 90 ? -0.5f : 0;
    }
    roll += (turn * 0.85f - roll) * 3.0f * DT;
    pitch += (climb * 0.45f - pitch) * 2.5f * DT;
    yaw += roll * 1.3f * DT;
    float ox = px + cosf(yaw) * PLANE_D, oy = py + sinf(yaw) * PLANE_D, oalt = alt - PLANE_DROP;
    float vx = cosf(yaw), vy = sinf(yaw);
    px += vx * speed * DT;
    py += vy * speed * DT;
    alt += sinf(pitch) * speed * DT / VS;
    if (alt > 520) alt = 520;
    if (sun_yaw > 0) sun_yaw += 0.00002f;

    /* the plane flies PLANE_D ahead of and PLANE_DROP below the chase camera */
    float qx = px + vx * PLANE_D, qy = py + vy * PLANE_D, qalt = alt - PLANE_DROP;
    float ground = ground_at(qx, qy), cam_ground = ground_at(px, py) + 3;
    pull_up = state == ST_PLAY && qalt - ground < 18 && pitch < 0.05f;
    if (qalt < ground + 1 && !invuln_t) { crash(); return; }
    if (qalt < ground + 1) alt = ground + 1 + PLANE_DROP;
    if (alt < cam_ground) alt = cam_ground;                  /* keep the camera out of the hills */

    if (state != ST_PLAY) return;
    time_left -= DT;
    if (time_left <= 0) { time_left = 0; game_over(); return; }
    Ring *r = &rings[0];
    float cx = cosf(r->dir), cy = sinf(r->dir);
    float before = (ox - r->x) * cx + (oy - r->y) * cy;
    float after = (qx - r->x) * cx + (qy - r->y) * cy;
    if (before < 0 && after >= 0) {
        float lat = -(qx - r->x) * cy + (qy - r->y) * cx;
        float ver = ((oalt + qalt) * 0.5f - r->alt) * VS;
        if (lat * lat + ver * ver < r->r * r->r) {
            nrings_done++;
            float bonus = 12.0f - nrings_done * 0.2f;
            time_left += bonus < 5 ? 5 : bonus;
            chime_f = 660; chime_t = 1; chime_n = 0;
            memmove(&rings[0], &rings[1], (MAX_RINGS - 1) * sizeof(Ring));
            place_ring(&rings[MAX_RINGS - 1], &rings[MAX_RINGS - 2]);
        }
    }
}

/* ============================================================ renderer */

static void build_sky(void)
{
    for (int i = -SKY_OFF; i < SKY_OFF; i++) {   /* index = pixels above the horizon */
        float t = i / 255.0f;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float s = sqrtf(t);
        sky[i] = RGB((int)(200 - 150 * s), (int)(220 - 120 * s), (int)(245 - 60 * s));
    }
}

/* blend a with b, k in 0..32 (32 = all a) */
static color_t blend(color_t a, uint32_t bspread, int k)
{
    uint32_t x = (a | ((uint32_t)a << 16)) & 0x07E0F81F;
    x = ((x * k + bspread * (32 - k)) >> 5) & 0x07E0F81F;
    return (color_t)(x | (x >> 16));
}

typedef struct { float depth; int idx; } RingView;

static void save_snap(int16_t *dst, int n) { for (int i = 0; i < n; i++) dst[i] = cols[i].y; }

static int row_fog, col_w;
static color_t *span_fb;
static uint32_t fog_spread;

/* draw the span from y down to the column's y-buffer in the map colour of idx, fogged */
static void fill_span(Col *c, int y, int idx)
{
    color_t k = row_fog >= 32 ? cmap[idx] : blend(cmap[idx], fog_spread, row_fog);
    int x = (int)(c - cols) * col_w;
    color_t *p = span_fb + y * 320 + x, *end = span_fb + c->y * 320 + x;
    c->y = (int16_t)y;
    if (col_w == 1) for (; p < end; p += 320) *p = k;
    else if (col_w == 2) for (; p < end; p += 320) { p[0] = k; p[1] = k; }
    else for (; p < end; p += 320) { p[0] = k; p[1] = k; p[2] = k; p[3] = k; }
}

/* The hot loop: one depth step, one map sample per screen column. Kept in a function of its own
 * with few locals so that pcc can hold them all in registers. y = horizon + (camh - h) / z. */
static void march_row(Col *c, Col *end, int fx, int fy, int sx, int sy, int invz, int base)
{
    for (; c < end; c++, fx += sx, fy += sy) {
        int idx = (((fy >> 16) & MAP_M) << MAP_SH) | ((fx >> 16) & MAP_M);
        int y = c->h + base - ((hmap[idx] * invz) >> 8);
        if (y < c->y) fill_span(c, y < 0 ? 0 : y, idx);
    }
}

/* sky rows 0..n-1 of one column; s points at the colour for row 0 and moves down the table */
static void sky_column(color_t *p, int n, const color_t *s)
{
    color_t *end = p + n * 320;
    if (col_w == 1) for (; p < end; p += 320) *p = *s--;
    else if (col_w == 2) for (; p < end; p += 320, s--) { p[0] = *s; p[1] = *s; }
    else for (; p < end; p += 320, s--) { color_t c = *s; p[0] = c; p[1] = c; p[2] = c; p[3] = c; }
}

static void render(void)
{
    color_t *fb = gfx_framebuffer();
    const Quality *q = &qual[quality];
    int ncol = q->cols, cw = q->colw;
    float sa = sinf(yaw), ca = cosf(yaw);
    float camroll = roll * 0.6f;
    float slope = -tanf(camroll);
    float hc = view_hc = VIEW_H * 0.42f + pitch * 110.0f;
    for (int i = 0; i < ncol; i++) {
        float xc = i * cw + cw * 0.5f - 160;
        float h = hc + xc * slope;
        cols[i].h = (int16_t)(h < -500 ? -500 : h > 500 ? 500 : h);
        cols[i].y = VIEW_H;
    }
    /* local map frame so the fixed-point coordinates stay small however far we fly */
    float bx = floorf(px / MAP_N) * MAP_N, by = floorf(py / MAP_N) * MAP_N;
    float lx0 = px - bx, ly0 = py - by;
    int camh16 = (int)(alt * 16);

    /* rings in view, nearest last so it draws on top */
    RingView rv[MAX_RINGS];
    int nrv = 0;
    if (state == ST_PLAY)
        for (int i = MAX_RINGS - 1; i >= 0; i--) {
            float dx = rings[i].x - px, dy = rings[i].y - py;
            float d = dx * ca + dy * sa;
            if (d > 2 && d < q->far) { rv[nrv].depth = d; rv[nrv].idx = i; nrv++; }
        }
    for (int a = 0; a < nrv; a++)
        for (int b = a + 1; b < nrv; b++)
            if (rv[b].depth > rv[a].depth) { RingView t = rv[a]; rv[a] = rv[b]; rv[b] = t; }
    int next_snap = nrv - 1;                     /* nearest first while marching outwards */

    color_t fogc = sky[0];
    fog_spread = (fogc | ((uint32_t)fogc << 16)) & 0x07E0F81F;
    col_w = cw;
    span_fb = fb;
    float far = q->far, z = 1.0f, dz = q->dz0;
    float k1x = ca + sa * FOVK, k1y = sa - ca * FOVK;           /* left edge direction */
    float k2x = -2 * sa * FOVK / ncol, k2y = 2 * ca * FOVK / ncol;   /* per column step */
    while (z < far) {
        while (next_snap >= 0 && rv[next_snap].depth <= z) { save_snap(snap[next_snap--], ncol); }
        int invz = (int)(SCALE * VS * 256.0f / z);
        float f = z / far;
        row_fog = (int)(32.5f - 32.0f * f * f * f);
        march_row(cols, cols + ncol, (int)((lx0 + z * k1x) * 65536.0f), (int)((ly0 + z * k1y) * 65536.0f),
                  (int)(z * k2x * 65536.0f), (int)(z * k2y * 65536.0f), invz, (camh16 * invz) >> 12);
        z += dz;
        dz += q->ddz;
    }
    while (next_snap >= 0) { save_snap(snap[next_snap--], ncol); }

    /* sky into whatever the terrain left */
    for (int i = 0; i < ncol; i++) sky_column(fb + i * cw, cols[i].y, sky + cols[i].h);
    /* sun: additive glow over sky pixels only */
    float rel = sun_yaw - yaw;
    while (rel > PI) rel -= 2 * PI;
    while (rel < -PI) rel += 2 * PI;
    if (fabsf(rel) < 1.0f) {
        int sxp = 160 + (int)(tanf(rel) * SCALE);
        int syp = (int)(hc + (sxp - 160) * slope) - 70;
        for (int y = syp - 26; y <= syp + 26; y++) {
            if (y < 0 || y >= VIEW_H) continue;
            for (int x = sxp - 26; x <= sxp + 26; x++) {
                if (x < 0 || x >= 320 || y >= cols[x / cw].y) continue;
                int d2 = (x - sxp) * (x - sxp) + (y - syp) * (y - syp);
                if (d2 > 676) continue;
                int add = d2 < 64 ? 255 : 9000 / (d2 + 10);
                color_t c = fb[y * 320 + x];
                int r = ((c >> 11) << 3) + add, g = (((c >> 5) & 63) << 2) + add, b = ((c & 31) << 3) + add * 3 / 4;
                fb[y * 320 + x] = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
            }
        }
    }

    /* rings, farthest first, each clipped by the y-buffer as it was at its depth */
    for (int n = 0; n < nrv; n++) {
        Ring *r = &rings[rv[n].idx];
        bool next = rv[n].idx == 0;
        color_t c = next ? ((sys_millis() / 150) & 1 ? YELLOW : ORANGE) : RGB(150, 120, 40);
        int16_t *sn = snap[n];
        float sxr = -sinf(r->dir), syr = cosf(r->dir);
        int lx = 0, ly = 0;
        bool have = false;
        for (int s = 0; s <= 32; s++) {
            float a = s * (2 * PI / 32);
            float wx = r->x + sxr * cosf(a) * r->r - px, wy = r->y + syr * cosf(a) * r->r - py;
            float wa = r->alt + sinf(a) * r->r / VS;
            float d = wx * ca + wy * sa, l = -wx * sa + wy * ca;
            if (d < 1.5f) { have = false; continue; }
            float xs = 160 + l * SCALE / d;
            if (xs < -400 || xs > 720) { have = false; continue; }
            int x = (int)xs;
            int y = (int)(hc + (xs - 160) * slope + (alt - wa) * SCALE * VS / d);
            if (y < -400 || y > 640) { have = false; continue; }
            if (have) {
                int dx = abs(x - lx), dy = -abs(y - ly), sxs = lx < x ? 1 : -1, sys = ly < y ? 1 : -1, err = dx + dy;
                int cx = lx, cy = ly, th = d < 60 ? 3 : d < 200 ? 2 : 1;
                for (;;) {
                    for (int oy = 0; oy < th; oy++)
                        for (int ox = 0; ox < th; ox++) {
                            int X = cx + ox, Y = cy + oy;
                            if ((unsigned)X < 320 && (unsigned)Y < VIEW_H && Y < sn[X / cw]) fb[Y * 320 + X] = c;
                        }
                    if (cx == x && cy == y) break;
                    int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; cx += sxs; }
                    if (e2 <= dx) { err += dx; cy += sys; }
                }
            }
            lx = x; ly = y; have = true;
        }
    }
}

static void draw_plane(void)
{
    if (crash_t) return;
    if (invuln_t && (invuln_t / 4) & 1) return;
    /* chase view from behind and above: the plane banks by what the camera does not */
    static const int8_t tri[][6] = {
        { 0, -10, -30, 4, 0, 1 },  { 0, -10, 30, 4, 0, 1 },     /* wings */
        { 0, -16, -4, 6, 4, 6 },                               /* fuselage */
        { 0, 0, -11, 7, 11, 7 },                               /* tailplane */
        { 0, -6, -1, 8, 1, 8 },                                /* fin */
    };
    static const color_t tc[] = { RGB(200, 60, 50), RGB(170, 45, 40), RGB(230, 230, 235), RGB(180, 50, 45), RGB(250, 250, 250) };
    float a = roll * 0.4f, c = cosf(a), s = sinf(a);
    int ox = 160, oy = (int)view_hc + PLANE_Y;
    for (int i = 0; i < 5; i++) {
        int X[3], Y[3];
        for (int j = 0; j < 3; j++) {
            float x = tri[i][j * 2], y = tri[i][j * 2 + 1];
            X[j] = ox + (int)(x * c - y * s);
            Y[j] = oy + (int)(x * s + y * c);
        }
        gfx_fill_triangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], tc[i]);
    }
}

static void draw_parts(void)
{
    color_t *fb = gfx_framebuffer();
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &parts[i];
        if (p->life <= 0) continue;
        int x = (int)p->x, y = (int)p->y, sz = p->life > 40 ? 3 : 2;
        for (int a = 0; a < sz; a++)
            for (int b = 0; b < sz; b++)
                if ((unsigned)(x + a) < 320 && (unsigned)(y + b) < VIEW_H) fb[(y + b) * 320 + x + a] = p->life < 15 ? DARKGRAY : p->c;
    }
}

static void center_text(int y, const char *s, color_t c)
{
    int x = 160 - gfx_text_width(s) / 2;
    gfx_text(x + 1, y + 1, s, BLACK);
    gfx_text(x, y, s, c);
}

static void draw_overlay(void)
{
    char buf[48];
    if (state == ST_TITLE) {
        gfx_font(FONT_7X13);
        center_text(40, "V O X E L   S K I E S", WHITE);
        gfx_font(FONT_6X10);
        center_text(64, "fly through the rings before time runs out", LIGHTGRAY);
        center_text(150, "LEFT/RIGHT bank  UP/DOWN pitch  a/z throttle", WHITE);
        center_text(164, "q quality  i invert  m mute  ESC quit", WHITE);
        if ((sys_millis() / 400) & 1) center_text(110, "press ENTER", YELLOW);
        snprintf(buf, sizeof buf, "best %d rings", best);
        center_text(84, buf, CYAN);
    } else if (state == ST_PLAY) {
        snprintf(buf, sizeof buf, "%d", nrings_done);
        gfx_font(FONT_7X13);
        center_text(6, buf, WHITE);
        gfx_font(FONT_6X10);
        if (pull_up && (sys_millis() / 200) & 1) center_text(120, "PULL UP", RED);
        if (time_left < 8 && (sys_millis() / 250) & 1) {
            snprintf(buf, sizeof buf, "%.1f s", (double)time_left);
            center_text(24, buf, RED);
        }
    } else {
        gfx_font(FONT_7X13);
        center_text(80, "TIME UP", RED);
        gfx_font(FONT_6X10);
        snprintf(buf, sizeof buf, "%d rings", nrings_done);
        center_text(104, buf, WHITE);
        if (new_best) center_text(120, "new best!", YELLOW);
        if (over_t > 90) center_text(150, "ENTER to continue", LIGHTGRAY);
    }
    if (flash_t) {
        color_t *fb = gfx_framebuffer();
        for (int i = 0; i < 320 * VIEW_H; i++) fb[i] = (fb[i] >> 1 & 0x7BEF) | 0x8410;
    }
}

static void draw_hud(void)
{
    char buf[48];
    gfx_fill_rect(0, HUD_Y, 320, 320 - HUD_Y, RGB(16, 20, 28));
    gfx_line(0, HUD_Y, 319, HUD_Y, RGB(70, 80, 100));
    int mx = 2, my = HUD_Y + 2;
    gfx_blit(mx, my, MINI, MINI, minimap);
    int wx = (int)(px - floorf(px / MAP_N) * MAP_N), wy = (int)(py - floorf(py / MAP_N) * MAP_N);
    int dx = mx + wx * MINI / MAP_N, dy = my + wy * MINI / MAP_N;
    if (state == ST_PLAY)
        for (int i = MAX_RINGS - 1; i >= 0; i--) {
            int rx = (int)(rings[i].x - floorf(rings[i].x / MAP_N) * MAP_N), ry = (int)(rings[i].y - floorf(rings[i].y / MAP_N) * MAP_N);
            gfx_fill_rect(mx + rx * MINI / MAP_N - 1, my + ry * MINI / MAP_N - 1, 3, 3, i ? RGB(150, 120, 40) : YELLOW);
        }
    gfx_line(dx, dy, dx + (int)(cosf(yaw) * 7), dy + (int)(sinf(yaw) * 7), WHITE);
    gfx_fill_rect(dx - 1, dy - 1, 3, 3, RED);

    int tx = 86, ty = HUD_Y + 5;
    if (state == ST_PLAY || state == ST_OVER) {
        gfx_printf(tx, ty, WHITE, "RINGS %d", nrings_done);
        gfx_printf(tx, ty + 12, time_left < 8 ? RED : WHITE, "TIME  %.1f", (double)time_left);
    } else {
        gfx_printf(tx, ty, WHITE, "BEST  %d", best);
        gfx_text(tx, ty + 12, "demo flight", GRAY);
    }
    float ground = ground_at(px + cosf(yaw) * PLANE_D, py + sinf(yaw) * PLANE_D);
    gfx_printf(tx, ty + 24, LIGHTGRAY, "SPD %3d", (int)(speed * 3.6f));
    gfx_printf(tx, ty + 36, LIGHTGRAY, "ALT %3d", (int)(alt - PLANE_DROP - ground));
    /* arrow to the next ring */
    if (state == ST_PLAY) {
        float rel = atan2f(rings[0].y - py, rings[0].x - px) - yaw;
        int ax = tx + 58, ay = ty + 56;
        gfx_circle(ax, ay, 11, DARKGRAY);
        gfx_line(ax, ay, ax + (int)(sinf(rel) * 10), ay - (int)(cosf(rel) * 10), YELLOW);
        gfx_fill_rect(ax + (int)(sinf(rel) * 10) - 1, ay - (int)(cosf(rel) * 10) - 1, 3, 3, YELLOW);
        float d = sqrtf((rings[0].x - px) * (rings[0].x - px) + (rings[0].y - py) * (rings[0].y - py));
        gfx_printf(tx, ty + 52, GRAY, "%3dm", (int)d);
    }

    int rx = 196;
    const Quality *q = &qual[quality];
    gfx_printf(rx, ty, fps >= 25 ? GREEN : fps >= 15 ? YELLOW : RED, "FPS %5.1f", (double)fps);
    gfx_printf(rx, ty + 12, LIGHTGRAY, "3D  %5.1f ms", (double)ms_render);
    gfx_printf(rx, ty + 24, LIGHTGRAY, "LCD %5.1f ms", (double)ms_lcd);
    snprintf(buf, sizeof buf, "Q %s %dx%d", q->name, q->cols, VIEW_H);
    gfx_text(rx, ty + 36, buf, CYAN);
    gfx_printf(rx, ty + 48, GRAY, "far %d", (int)q->far);
    gfx_printf(rx, ty + 60, GRAY, "heap %uK", (unsigned)(sys_free_heap() / 1024));
}

/* ============================================================ main */

void setup(void)
{
    gfx_clear(BLACK);
    gfx_font(FONT_7X13);
    center_text(150, "growing terrain...", WHITE);
    gfx_font(FONT_6X10);
    gfx_flush();
    hmap = malloc(MAP_N * MAP_N);
    cmap = malloc(MAP_N * MAP_N * sizeof(color_t));
    if (!hmap || !cmap) {
        gfx_clear(BLACK);
        gfx_text(10, 10, "not enough memory", RED);
        gfx_flush();
        sys_delay(2000);
        sys_exit();
    }
    FILE *f = fopen(HISCORE_FILE, "r");
    if (f) { if (fscanf(f, "%d", &best) != 1) best = 0; fclose(f); }
    sys_seed(sys_millis() ^ 0x1234567);
    gen_terrain();
    build_sky();
    spawn_plane();
    snd_volume(6);
    gfx_clear(BLACK);
    draw_hud();
    gfx_flush();
    t0 = last_ms = hud_ms = sys_millis();
}

void loop(void)
{
    int k;
    while ((k = kbd_get()) != 0) {
        if (k == KEY_ESC) { snd_stop(); sys_exit(); }
        if (k == 'q') quality = (quality + 1) % NQUAL;
        if (k == 'i') inverted = !inverted;
        if (k == 'm') muted = !muted;
        if (k == KEY_ENTER && state == ST_TITLE) new_game();
        else if (k == KEY_ENTER && state == ST_OVER && over_t > 90) { state = ST_TITLE; spawn_plane(); }
    }

    uint32_t now = sys_millis();
    int steps = 0;
    while (now - last_ms >= STEP_MS && steps < 4) { step(); last_ms += STEP_MS; steps++; }
    if (now - last_ms > STEP_MS * 4) last_ms = now;
    pump_audio(now - t0);

    uint64_t a = sys_micros();
    render();
    draw_plane();
    draw_parts();
    draw_overlay();
    uint64_t b = sys_micros();
    gfx_flush_rect(0, 0, 320, VIEW_H);
    uint64_t c = sys_micros();
    float r_ms = (b - a) / 1000.0f, l_ms = (c - b) / 1000.0f;
    ms_render += (r_ms - ms_render) * 0.1f;
    ms_lcd += (l_ms - ms_lcd) * 0.1f;
    static uint64_t prev;
    if (prev) {
        float inst = 1e6f / (float)(c - prev);
        fps += (inst - fps) * 0.1f;
    }
    prev = c;
    if (now - hud_ms >= 250) {
        hud_ms = now;
        draw_hud();
        gfx_flush_rect(0, HUD_Y, 320, 320 - HUD_Y);
    }
}
