/* kart - a mode-7 kart race: three laps against five computer drivers.
 * The ground is a 512x512 texture (track, kerbs, sand, grass, a lake) built at start-up from a
 * spline. Each screen row below the horizon is one straight line through that texture, so it is
 * drawn with two fixed-point adds and a lookup per pixel, then fogged with a per-row palette.
 * The karts are small 3D box models rendered at start-up from 16 angles into sprites; the trees
 * are procedural billboards. Sprites are scaled, sorted far to near and drawn with shadows.
 * The bottom strip shows the race, a map and where the frame time goes for each quality level.
 *
 *   UP accelerate  DOWN brake  LEFT/RIGHT steer  z drift  q quality  m mute  ENTER start  ESC quit */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_SH 9
#define TEX_N (1 << TEX_SH)
#define TEX_M (TEX_N - 1)
#define VIEW_H 240
#define HUD_Y VIEW_H
#define HZ 72                         /* horizon row */
#define FOC 160.0f                    /* focal length in pixels: 90 degree view */
#define CAM_H 7.0f
#define CAM_D 17.0f
#define STEP_MS 16
#define DT (STEP_MS / 1000.0f)
#define PI 3.14159265f
#define NKARTS 6
#define LAPS 3
#define MAX_SAMPLES 640
#define SPACING 4.0f                  /* centre line sample spacing */
#define ROAD_W 17.0f                  /* half width of the tarmac */
#define KERB_W 3.0f
#define NTREES 70
#define NANG 16
#define KSPR_W 48
#define KSPR_H 40
#define KSPR_AX 24
#define KSPR_AY 28
#define KPPU 5.0f                     /* kart sprite pixels per world unit */
#define TSPR_W 32
#define TSPR_H 48
#define TPPU (48.0f / 24.0f)          /* trees are 24 units tall */
#define KEYC 0xF81F                   /* transparent sprite colour */
#define MINI 76
#define HISCORE_FILE SD_ROOT "/pico-dev/kart.hi"

enum { P_GRASS, P_GRASS2, P_ROAD, P_ROAD2, P_KERB_R, P_KERB_W, P_SAND, P_LINE_B, P_LINE_W,
       P_BOOST, P_BOOST2, P_WATER, P_WATER2, P_DIRT, NPAL };

typedef struct { int cols; float far; int trees; bool ss; const char *name; } Quality;

static const Quality qual[] = {
    { 160, 260.0f, 0, false, "LOW" },
    { 320, 340.0f, 25, false, "MED" },
    { 320, 420.0f, NTREES, false, "HIGH" },
    { 320, 480.0f, NTREES, true, "ULTRA" },
};
#define NQUAL 4

typedef struct {
    float x, y, a, v;          /* position, heading, speed */
    float t, lane, lane_to;    /* AI: position along the centre line and lateral offset */
    float skill;
    int idx, lap;              /* nearest centre line sample, laps started (0 = on the grid) */
    float finish_time;
    float bump;
    color_t body, suit;
} Kart;

typedef struct { float x, y; } Tree;
typedef struct { float depth, sx, sy, scale; int kind, n; } Draw;

enum { ST_TITLE, ST_COUNT, ST_RACE, ST_DONE };

static uint8_t *track;
static color_t fogpal[33][NPAL];
static color_t *kspr;                 /* [NKARTS][NANG][KSPR_H * KSPR_W] */
static color_t tspr[TSPR_W * TSPR_H];
static color_t minimap[MINI * MINI];
static color_t sky_row[HZ];
static uint8_t mount1[512], mount2[512];
static float cl_x[MAX_SAMPLES], cl_y[MAX_SAMPLES], cl_a[MAX_SAMPLES], cl_spd[MAX_SAMPLES];
static int nsamp;
static Kart karts[NKARTS];
static Tree trees[NTREES];
static Draw draws[NKARTS + NTREES];

static int state = ST_TITLE, quality = 1, count_t, finish_order, player_place;
static float cam_x, cam_y, cam_a, race_time, lap_start, best_lap, best_race, last_lap, boost_t, shake;
static bool muted, new_best, drifting, offroad;
static uint32_t last_ms, t0, hud_ms;
static float fps, ms_render, ms_lcd;

static float frand(float a, float b) { return a + (b - a) * (float)(sys_random() & 0xFFFF) / 65535.0f; }

static float wrap_angle(float a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static int tex_at(float x, float y) { return track[(((int)y & TEX_M) << TEX_SH) | ((int)x & TEX_M)]; }

/* ============================================================ track */

static const float ctrl[][2] = {
    { 110.0f, 96.0f }, { 250.0f, 72.0f }, { 400.0f, 84.0f }, { 456.0f, 160.0f }, { 420.0f, 226.0f },
    { 330.0f, 214.0f }, { 268.0f, 254.0f }, { 318.0f, 316.0f }, { 432.0f, 338.0f }, { 452.0f, 430.0f },
    { 360.0f, 466.0f }, { 220.0f, 446.0f }, { 150.0f, 380.0f }, { 160.0f, 290.0f }, { 84.0f, 236.0f },
    { 62.0f, 150.0f },
};
#define NCTRL 16

static void spline(int seg, float t, float *x, float *y)
{
    const float *p0 = ctrl[(seg + NCTRL - 1) % NCTRL], *p1 = ctrl[seg], *p2 = ctrl[(seg + 1) % NCTRL],
                *p3 = ctrl[(seg + 2) % NCTRL];
    float t2 = t * t, t3 = t2 * t;
    *x = 0.5f * (2 * p1[0] + (p2[0] - p0[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 + (3 * p1[0] - p0[0] - 3 * p2[0] + p3[0]) * t3);
    *y = 0.5f * (2 * p1[1] + (p2[1] - p0[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 + (3 * p1[1] - p0[1] - 3 * p2[1] + p3[1]) * t3);
}

/* centre line resampled to equal steps, with heading and a safe speed from the curvature */
static void build_centre_line(void)
{
    float px, py, acc = 0;
    spline(0, 0, &px, &py);
    cl_x[0] = px; cl_y[0] = py;
    nsamp = 1;
    for (int s = 0; s < NCTRL; s++)
        for (int i = 1; i <= 64; i++) {
            float x, y;
            spline(s, i / 64.0f, &x, &y);
            float d = sqrtf((x - px) * (x - px) + (y - py) * (y - py));
            acc += d;
            while (acc >= SPACING && nsamp < MAX_SAMPLES) {
                acc -= SPACING;
                float k = 1 - acc / d;
                cl_x[nsamp] = px + (x - px) * k;
                cl_y[nsamp] = py + (y - py) * k;
                nsamp++;
            }
            px = x; py = y;
        }
    if (nsamp > 2) nsamp--;                    /* the last one coincides with the first */
    for (int i = 0; i < nsamp; i++) {
        int j = (i + 1) % nsamp;
        cl_a[i] = atan2f(cl_y[j] - cl_y[i], cl_x[j] - cl_x[i]);
    }
    for (int i = 0; i < nsamp; i++) {
        float turn = 0;                        /* how much the road bends over the next stretch */
        for (int k = 0; k < 10; k++) turn += fabsf(wrap_angle(cl_a[(i + k + 1) % nsamp] - cl_a[(i + k) % nsamp]));
        float s = 1.0f - turn * 0.55f;
        cl_spd[i] = s < 0.45f ? 0.45f : s;
    }
}

static void build_track(void)
{
    uint8_t *dist = malloc(TEX_N * TEX_N), *par = malloc(TEX_N * TEX_N);
    memset(dist, 255, TEX_N * TEX_N);
    int R = 40;
    for (int i = 0; i < nsamp; i++) {          /* stamp distance-to-centre discs along the line */
        int cx = (int)cl_x[i], cy = (int)cl_y[i];
        for (int y = -R; y <= R; y++)
            for (int x = -R; x <= R; x++) {
                int d2 = x * x + y * y;
                if (d2 > R * R) continue;
                int X = cx + x, Y = cy + y;
                if ((unsigned)X >= TEX_N || (unsigned)Y >= TEX_N) continue;
                int d = (int)(sqrtf((float)d2) * 4);
                int o = (Y << TEX_SH) | X;
                if (d < dist[o]) { dist[o] = (uint8_t)d; par[o] = (uint8_t)i; }
            }
    }
    /* the lake sits in the big infield away from the road */
    for (int y = 0; y < TEX_N; y++)
        for (int x = 0; x < TEX_N; x++) {
            int o = (y << TEX_SH) | x;
            float d = dist[o] / 4.0f;
            int p = par[o];
            int n = (int)(sys_random() & 7);
            uint8_t c;
            if (d < ROAD_W) {
                c = n < 2 ? P_ROAD2 : P_ROAD;
                if (p < 3 && p >= 1) c = (((x >> 2) + (y >> 2)) & 1) ? P_LINE_W : P_LINE_B;   /* chequered line */
                bool boost = (p % 150 >= 70 && p % 150 < 76) && d < 7;
                if (boost) c = ((p + (int)(d > 3.5f)) & 1) ? P_BOOST : P_BOOST2;
            } else if (d < ROAD_W + KERB_W) c = ((p >> 1) & 1) ? P_KERB_R : P_KERB_W;
            else if (d < ROAD_W + KERB_W + 5 + (n & 3)) c = P_SAND;
            else {
                float lx = x - 300.0f, ly = y - 130.0f;
                float lake = lx * lx / 3600.0f + ly * ly / 1100.0f + sinf(x * 0.09f) * 0.15f;
                if (lake < 1.0f && d > 34) c = n < 3 ? P_WATER2 : P_WATER;
                else if (lake < 1.25f && d > 30) c = P_DIRT;
                else c = (((x >> 4) + (y >> 4)) & 1) ? P_GRASS : P_GRASS2;   /* mown stripes */
            }
            track[o] = c;
        }
    /* trees on grass, not too close to the road */
    int nt = 0;
    for (int tries = 0; nt < NTREES && tries < 5000; tries++) {
        int x = 10 + (int)(sys_random() % (TEX_N - 20)), y = 10 + (int)(sys_random() % (TEX_N - 20));
        int o = (y << TEX_SH) | x;
        if (dist[o] / 4 < 34) continue;
        int c = track[o];
        if (c != P_GRASS && c != P_GRASS2) continue;
        trees[nt].x = (float)x; trees[nt].y = (float)y;
        nt++;
    }
    for (; nt < NTREES; nt++) { trees[nt].x = 2; trees[nt].y = 2; }
    free(dist);
    free(par);
}

static const uint8_t base_rgb[NPAL][3] = {
    { 60, 150, 50 }, { 72, 168, 58 }, { 96, 96, 104 }, { 104, 104, 112 }, { 220, 40, 40 }, { 240, 240, 240 },
    { 214, 196, 140 }, { 20, 20, 20 }, { 245, 245, 245 }, { 255, 210, 0 }, { 255, 120, 0 },
    { 40, 110, 200 }, { 70, 140, 220 }, { 150, 120, 80 },
};

static void build_palettes(void)
{
    const int fr = 170, fg = 200, fb = 235;    /* haze at the horizon */
    for (int l = 0; l <= 32; l++)
        for (int c = 0; c < NPAL; c++) {
            int r = (base_rgb[c][0] * l + fr * (32 - l)) / 32;
            int g = (base_rgb[c][1] * l + fg * (32 - l)) / 32;
            int b = (base_rgb[c][2] * l + fb * (32 - l)) / 32;
            fogpal[l][c] = RGB(r, g, b);
        }
    for (int y = 0; y < HZ; y++) {
        float t = (float)y / HZ;
        sky_row[y] = RGB((int)(60 + 110 * t), (int)(120 + 80 * t), (int)(210 + 25 * t));
    }
    for (int i = 0; i < 512; i++) {            /* two mountain ranges for the backdrop */
        float a = i * 2 * PI / 512;
        mount1[i] = (uint8_t)(20 + 10 * sinf(a * 3) + 7 * sinf(a * 7 + 1) + 4 * sinf(a * 17 + 2));
        mount2[i] = (uint8_t)(11 + 5 * sinf(a * 5 + 3) + 3 * sinf(a * 13) + 2 * sinf(a * 31 + 1));
    }
    for (int y = 0; y < MINI; y++)
        for (int x = 0; x < MINI; x++) {
            int c = track[((y * TEX_N / MINI) << TEX_SH) | (x * TEX_N / MINI)];
            minimap[y * MINI + x] = c == P_ROAD || c == P_ROAD2 || (c >= P_LINE_B && c <= P_BOOST2) ? RGB(150, 150, 160)
                                  : (fogpal[32][c] >> 1) & 0x7BEF;
        }
}

/* ============================================================ sprites built at start-up */

typedef struct { float x0, x1, y0, y1, z0, z1; int col; } Box;

/* kart frame: x forward, y right, z up; col 0 body, 1 dark body, 2 tyre, 3 metal, 4 suit, 5 helmet */
static const Box kart_model[] = {
    { -3.2f, 3.0f, -2.0f, 2.0f, 0.4f, 1.0f, 0 },
    { 3.0f, 3.8f, -1.4f, 1.4f, 0.4f, 0.9f, 1 },
    { -3.6f, -2.4f, -1.2f, 1.2f, 0.6f, 1.8f, 3 },
    { 1.6f, 2.8f, -2.9f, -1.9f, 0.0f, 1.2f, 2 },
    { 1.6f, 2.8f, 1.9f, 2.9f, 0.0f, 1.2f, 2 },
    { -2.8f, -1.4f, -3.0f, -1.9f, 0.0f, 1.4f, 2 },
    { -2.8f, -1.4f, 1.9f, 3.0f, 0.0f, 1.4f, 2 },
    { -1.8f, -0.4f, -0.9f, 0.9f, 1.0f, 2.4f, 4 },
    { -1.4f, -0.2f, -0.7f, 0.7f, 2.4f, 3.4f, 5 },
    { -3.9f, -3.1f, -2.3f, 2.3f, 2.0f, 2.4f, 1 },
    { -3.6f, -3.3f, -0.8f, 0.8f, 1.0f, 2.0f, 3 },
};
#define NBOX 11

typedef struct { float sx[4], sy[4], depth; color_t c; } Face;

static color_t shade(color_t c, float k)
{
    int r = (int)(((c >> 11) << 3) * k), g = (int)((((c >> 5) & 63) << 2) * k), b = (int)(((c & 31) << 3) * k);
    color_t o = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    return o == KEYC ? (color_t)(KEYC ^ 0x20) : o;
}

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void fill_tri(color_t *dst, int w, int h, float x0, float y0, float x1, float y1, float x2, float y2, color_t c)
{
    float area = edge(x0, y0, x1, y1, x2, y2);
    if (fabsf(area) < 1e-3f) return;
    int minx = (int)floorf(minf(x0, minf(x1, x2))), maxx = (int)ceilf(maxf(x0, maxf(x1, x2)));
    int miny = (int)floorf(minf(y0, minf(y1, y2))), maxy = (int)ceilf(maxf(y0, maxf(y1, y2)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > w - 1) maxx = w - 1;
    if (maxy > h - 1) maxy = h - 1;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float a = edge(x1, y1, x2, y2, px, py), b = edge(x2, y2, x0, y0, px, py), d = edge(x0, y0, x1, y1, px, py);
            if (area < 0) { a = -a; b = -b; d = -d; }
            if (a >= 0 && b >= 0 && d >= 0) dst[y * w + x] = c;
        }
}

static void render_kart(color_t *dst, float rel, const color_t *cols)
{
    static const int8_t faces[6][4][3] = {     /* corner selectors (x, y, z: 0 = min, 1 = max) */
        { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } }, { { 0, 1, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 } },
        { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } }, { { 1, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 1, 0, 1 } },
        { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } }, { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } },
    };
    static const float nrm[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    Face f[NBOX * 6];
    int nf = 0;
    float cr = cosf(rel), sr = sinf(rel), cp = cosf(0.36f), sp = sinf(0.36f);
    for (int i = 0; i < KSPR_W * KSPR_H; i++) dst[i] = KEYC;
    for (int b = 0; b < NBOX; b++) {
        const Box *bx = &kart_model[b];
        for (int k = 0; k < 6; k++) {
            /* face normal in the view: depth component D and up component */
            float nx = nrm[k][0], ny = nrm[k][1], nz = nrm[k][2];
            float nD = nx * cr - ny * sr, nU = nz;
            float toward = -(nD * cp - nU * sp);   /* > 0: faces the camera */
            if (toward <= 0.01f) continue;
            float nR = nx * sr + ny * cr;
            float light = 0.55f + 0.45f * maxf(0.0f, nU * 0.8f - nR * 0.35f - nD * 0.45f);
            Face *F = &f[nf++];
            F->depth = 0;
            for (int v = 0; v < 4; v++) {
                float x = faces[k][v][0] ? bx->x1 : bx->x0, y = faces[k][v][1] ? bx->y1 : bx->y0, z = faces[k][v][2] ? bx->z1 : bx->z0;
                float D = x * cr - y * sr, R = x * sr + y * cr;
                F->sx[v] = KSPR_AX + R * KPPU;
                F->sy[v] = KSPR_AY - (z * cp + D * sp) * KPPU;
                F->depth += D * cp - z * sp;
            }
            F->c = shade(cols[bx->col], light);
        }
    }
    for (int a = 0; a < nf; a++)               /* painter's order, farthest first */
        for (int b = a + 1; b < nf; b++)
            if (f[b].depth > f[a].depth) { Face t = f[a]; f[a] = f[b]; f[b] = t; }
    for (int i = 0; i < nf; i++) {
        fill_tri(dst, KSPR_W, KSPR_H, f[i].sx[0], f[i].sy[0], f[i].sx[1], f[i].sy[1], f[i].sx[2], f[i].sy[2], f[i].c);
        fill_tri(dst, KSPR_W, KSPR_H, f[i].sx[0], f[i].sy[0], f[i].sx[2], f[i].sy[2], f[i].sx[3], f[i].sy[3], f[i].c);
    }
}

static void build_tree(void)
{
    for (int i = 0; i < TSPR_W * TSPR_H; i++) tspr[i] = KEYC;
    for (int y = 34; y < 48; y++)
        for (int x = 14; x < 18; x++) tspr[y * TSPR_W + x] = x == 14 ? RGB(70, 45, 25) : RGB(110, 75, 40);
    for (int layer = 0; layer < 3; layer++) {      /* three stacked round crowns */
        float cx = 16, cy = 30 - layer * 10.0f, r = 13 - layer * 3.0f;
        for (int y = 0; y < TSPR_H; y++)
            for (int x = 0; x < TSPR_W; x++) {
                float dx = x + 0.5f - cx, dy = (y + 0.5f - cy) * 1.2f;
                if (dx * dx + dy * dy > r * r) continue;
                float lit = 0.55f + 0.45f * (-(dx + dy) / (r * 1.5f) + 0.5f) + (float)(sys_random() % 100) / 500.0f;
                if (lit > 1.2f) lit = 1.2f;
                tspr[y * TSPR_W + x] = shade(RGB(40, 120, 45), lit);
            }
    }
}

static const color_t kart_body[NKARTS] = { RGB(220, 30, 30), RGB(30, 90, 230), RGB(250, 200, 0), RGB(40, 180, 60), RGB(240, 120, 0), RGB(170, 60, 200) };

static void build_karts(void)
{
    for (int k = 0; k < NKARTS; k++) {
        color_t b = kart_body[k];
        color_t cols[6] = { b, shade(b, 0.6f), RGB(30, 30, 34), RGB(150, 150, 160), shade(b, 0.8f), k == 0 ? WHITE : RGB(240, 240, 200) };
        for (int a = 0; a < NANG; a++) render_kart(kspr + (k * NANG + a) * KSPR_W * KSPR_H, a * 2 * PI / NANG, cols);
    }
}

/* ============================================================ sound */

#define RATE 11025
#define LEAD_SAMPLES (RATE / 8)
#define BLOCK 256
static int16_t audio[BLOCK];
static uint32_t sent;
static float eng_phase, eng2_phase, beep_f, beep_t, beep_phase, lp;

static void beep(float f, float len) { beep_f = f; beep_t = len; }

static void synth(int16_t *out, int n)
{
    Kart *p = &karts[0];
    float ef = 50 + fabsf(p->v) * 2.2f + (boost_t > 0 ? 30 : 0);
    for (int i = 0; i < n; i++) {
        float s = 0;
        if (state != ST_TITLE) {
            eng_phase += ef / RATE;
            if (eng_phase >= 1) eng_phase -= 1;
            eng2_phase += ef * 1.505f / RATE;
            if (eng2_phase >= 1) eng2_phase -= 1;
            s += (eng_phase < 0.3f ? 0.12f : -0.05f) + (eng2_phase < 0.5f ? 0.04f : -0.04f);
            if (drifting || offroad) {
                float noise = (float)(int)(sys_random() & 0xFFF) / 2048.0f - 1;
                lp += (noise - lp) * (offroad ? 0.05f : 0.5f);
                s += lp * (offroad ? 0.5f : 0.10f);
            }
        }
        if (beep_t > 0) {
            beep_phase += beep_f / RATE;
            if (beep_phase >= 1) beep_phase -= 1;
            s += beep_phase < 0.5f ? 0.2f : -0.2f;
            beep_t -= 1.0f / RATE;
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

/* ============================================================ race logic */

static void cl_point(float t, float lane, float *x, float *y, float *a)
{
    int i = (int)t;
    float f = t - i;
    i %= nsamp;
    int j = (i + 1) % nsamp;
    float cx = cl_x[i] + (cl_x[j] - cl_x[i]) * f, cy = cl_y[i] + (cl_y[j] - cl_y[i]) * f;
    float an = cl_a[i] + wrap_angle(cl_a[j] - cl_a[i]) * f;
    *x = cx - sinf(an) * lane;
    *y = cy + cosf(an) * lane;
    *a = an;
}

static int nearest_sample(float x, float y, int around)
{
    int best = around;
    float bd = 1e30f;
    for (int k = -24; k <= 24; k++) {
        int i = (around + k + nsamp) % nsamp;
        float d = (cl_x[i] - x) * (cl_x[i] - x) + (cl_y[i] - y) * (cl_y[i] - y);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static float progress(const Kart *k) { return (float)k->lap * nsamp + k->idx; }

static void place_grid(void)
{
    for (int i = 0; i < NKARTS; i++) {
        Kart *k = &karts[i];
        int slot = NKARTS - 1 - i;               /* the player starts at the back */
        float t = (float)(nsamp - 6 - slot * 7);
        k->t = t;
        k->lane = k->lane_to = (slot & 1) ? 6.0f : -6.0f;
        cl_point(t, k->lane, &k->x, &k->y, &k->a);
        k->v = 0;
        k->idx = (int)t;
        k->lap = 0;
        k->finish_time = 0;
        k->bump = 0;
        k->skill = i == 0 ? 1.0f : 0.80f + i * 0.025f;
        k->body = kart_body[i];
    }
    cam_a = karts[0].a;
    cam_x = karts[0].x - cosf(cam_a) * CAM_D;
    cam_y = karts[0].y - sinf(cam_a) * CAM_D;
}

static void count_lap(Kart *k, int old_idx)
{
    if (old_idx > nsamp * 3 / 4 && k->idx < nsamp / 4) {
        k->lap++;
        if (k == &karts[0] && state == ST_RACE) {
            if (k->lap > 1) {
                last_lap = race_time - lap_start;
                if (best_lap == 0 || last_lap < best_lap) best_lap = last_lap;
                beep(1320, 0.15f);
            }
            lap_start = race_time;
        }
        if (k->lap > LAPS && k->finish_time == 0) {
            k->finish_time = race_time;
            finish_order++;
            if (k == &karts[0]) {
                player_place = finish_order;
                state = ST_DONE;
                count_t = 0;
                if (best_race == 0 || race_time < best_race) { best_race = race_time; new_best = true; }
                FILE *f = fopen(HISCORE_FILE, "w");
                if (f) { fprintf(f, "%d %d\n", (int)(best_race * 1000), (int)(best_lap * 1000)); fclose(f); }
                beep(880, 0.6f);
            }
        }
    } else if (k->idx > nsamp * 3 / 4 && old_idx < nsamp / 4)
        k->lap--;                                /* went back over the line */
}

static void ai_drive(Kart *k, float top)
{
    /* follow the centre line at a lane offset, slowing for bends */
    float want = top * k->skill * cl_spd[((int)k->t + 4) % nsamp];
    Kart *p = &karts[0];
    float lead = progress(k) - progress(p);
    if (k != p && state == ST_RACE) want *= lead > 60 ? 0.92f : lead < -60 ? 1.06f : 1.0f;   /* keep the pack together */
    k->v += (want - k->v) * (want > k->v ? 0.6f : 2.0f) * DT;
    if (sys_random() % 240 == 0) k->lane_to = frand(-ROAD_W + 5, ROAD_W - 5);
    k->lane += (k->lane_to - k->lane) * 0.8f * DT;
    float ox = k->x, oy = k->y;
    k->t += k->v * DT / SPACING;
    if (k->t >= nsamp) k->t -= nsamp;
    cl_point(k->t, k->lane, &k->x, &k->y, &k->a);
    int old = k->idx;
    k->idx = (int)k->t;
    count_lap(k, old);
    (void)ox; (void)oy;
}

static void player_drive(Kart *k)
{
    float steer = 0, throttle = 0;
    bool auto_drive = state == ST_TITLE || state == ST_DONE;
    if (auto_drive) {
        float tx, ty, ta;
        cl_point((float)((k->idx + 6) % nsamp), 0, &tx, &ty, &ta);
        float rel = wrap_angle(atan2f(ty - k->y, tx - k->x) - k->a);
        steer = rel * 3;
        if (steer > 1) steer = 1;
        if (steer < -1) steer = -1;
        throttle = k->v < 62 * cl_spd[(k->idx + 4) % nsamp] ? 1 : 0;
        drifting = false;
    } else if (state == ST_RACE) {
        steer = (kbd_down(KEY_RIGHT) ? 1.0f : 0.0f) - (kbd_down(KEY_LEFT) ? 1.0f : 0.0f);
        throttle = kbd_down(KEY_UP) ? 1.0f : kbd_down(KEY_DOWN) ? -1.0f : 0.0f;
        drifting = kbd_down('z') && k->v > 30 && steer != 0;
    }
    int c = tex_at(k->x, k->y);
    offroad = c == P_GRASS || c == P_GRASS2 || c == P_SAND || c == P_WATER || c == P_WATER2 || c == P_DIRT;
    if (c == P_BOOST || c == P_BOOST2) { if (boost_t <= 0) beep(1760, 0.08f); boost_t = 1.2f; }
    float top = offroad ? 32.0f : 72.0f;
    if (boost_t > 0) { top = 100; boost_t -= DT; throttle = 1; }
    if (throttle > 0) k->v += (top - k->v) * 0.55f * DT * throttle + (k->v < top ? 4 : 0) * DT;
    else if (throttle < 0) k->v -= 60 * DT;
    else k->v -= k->v * 0.6f * DT;
    if (k->v > top) k->v -= (k->v - top) * 2.5f * DT;
    if (k->v < -15) k->v = -15;
    float grip = drifting ? 2.1f : 1.4f;
    k->a = wrap_angle(k->a + steer * grip * DT * (k->v / 45.0f > 1 ? 1 : k->v / 45.0f) * (k->v < 0 ? -1 : 1));
    /* a drift carries the kart along its old line for a moment */
    float slip = drifting ? steer * 0.35f : 0;
    float dir = k->a - slip;
    k->x += cosf(dir) * k->v * DT;
    k->y += sinf(dir) * k->v * DT;
    if (k->x < 8) k->x = 8;
    if (k->y < 8) k->y = 8;
    if (k->x > TEX_N - 8) k->x = TEX_N - 8;
    if (k->y > TEX_N - 8) k->y = TEX_N - 8;
    int old = k->idx;
    k->idx = nearest_sample(k->x, k->y, k->idx);
    count_lap(k, old);
}

static void collide(void)
{
    for (int i = 0; i < NKARTS; i++)
        for (int j = i + 1; j < NKARTS; j++) {
            Kart *a = &karts[i], *b = &karts[j];
            float dx = b->x - a->x, dy = b->y - a->y, d2 = dx * dx + dy * dy;
            if (d2 > 36 || d2 < 1e-4f) continue;
            float d = sqrtf(d2), push = (6 - d) * 0.5f;
            dx /= d; dy /= d;
            if (i == 0) {                        /* the player is moved, the AI keep their line */
                a->x -= dx * push * 2; a->y -= dy * push * 2;
                a->v *= 0.85f;
                b->lane_to = b->lane + (b->lane > 0 ? -4.0f : 4.0f);
                shake = 4;
                a->bump = 0.3f;
            } else {
                a->lane_to += -3; b->lane_to += 3;
                if (a->lane_to < -ROAD_W + 4) a->lane_to = -ROAD_W + 4;
                if (b->lane_to > ROAD_W - 4) b->lane_to = ROAD_W - 4;
            }
        }
}

static void new_race(void)
{
    place_grid();
    state = ST_COUNT;
    count_t = 0;
    race_time = lap_start = last_lap = 0;
    best_lap = 0;
    finish_order = 0;
    player_place = 0;
    new_best = false;
    boost_t = 0;
}

static void step(void)
{
    if (state == ST_COUNT) {
        count_t++;
        if (count_t % 60 == 0 && count_t < 240) beep(count_t == 180 ? 1320 : 660, count_t == 180 ? 0.35f : 0.15f);
        if (count_t >= 180) { state = ST_RACE; race_time = 0; }
    } else {
        if (state == ST_RACE) race_time += DT;
        if (state == ST_DONE) count_t++;
        for (int i = 1; i < NKARTS; i++) ai_drive(&karts[i], 72);
        player_drive(&karts[0]);
        collide();
    }
    if (state == ST_TITLE) {
        /* keep the attract-mode AI circulating forever */
        for (int i = 1; i < NKARTS; i++) if (karts[i].lap > 50) karts[i].lap = 1;
    }
    Kart *p = &karts[0];
    if (p->bump > 0) p->bump -= DT;
    if (shake > 0) shake -= 0.5f;
    /* chase camera, lagging a little behind the kart's heading */
    cam_a = wrap_angle(cam_a + wrap_angle(p->a - cam_a) * 0.12f);
    float tx = p->x - cosf(cam_a) * CAM_D, ty = p->y - sinf(cam_a) * CAM_D;
    cam_x += (tx - cam_x) * 0.5f;
    cam_y += (ty - cam_y) * 0.5f;
    player_place = 1;
    if (state != ST_DONE)
        for (int i = 1; i < NKARTS; i++) if (progress(&karts[i]) > progress(p)) player_place++;
}

/* ============================================================ renderer */

/* the hot loops: one straight line through the track texture per screen row */
static void m7_row(color_t *p, color_t *end, int u, int v, int du, int dv, const color_t *pal)
{
    const uint8_t *tx = track;
    for (; p < end; p++, u += du, v += dv) *p = pal[tx[(((v >> 16) & TEX_M) << TEX_SH) | ((u >> 16) & TEX_M)]];
}

static void m7_row2(color_t *p, color_t *end, int u, int v, int du, int dv, const color_t *pal)
{
    const uint8_t *tx = track;
    for (; p < end; p += 2, u += du, v += dv) p[0] = p[1] = pal[tx[(((v >> 16) & TEX_M) << TEX_SH) | ((u >> 16) & TEX_M)]];
}

/* two samples per pixel, averaged: less shimmer in the distance at twice the cost */
static void m7_row_ss(color_t *p, color_t *end, int u, int v, int du, int dv, const color_t *pal)
{
    const uint8_t *tx = track;
    int hu = du >> 1, hv = dv >> 1;
    for (; p < end; p++, u += du, v += dv) {
        color_t a = pal[tx[(((v >> 16) & TEX_M) << TEX_SH) | ((u >> 16) & TEX_M)]];
        color_t b = pal[tx[((((v + hv) >> 16) & TEX_M) << TEX_SH) | (((u + hu) >> 16) & TEX_M)]];
        *p = (color_t)(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1));
    }
}

static void draw_backdrop(color_t *fb, float ca)
{
    int off = (int)(ca * 512 / (2 * PI)) & 511;
    color_t far_c = RGB(120, 140, 175), near_c = RGB(60, 110, 70);
    for (int y = 0; y < HZ; y++) {
        color_t *p = fb + y * 320, c = sky_row[y];
        for (int x = 0; x < 320; x++) p[x] = c;
    }
    for (int x = 0; x < 320; x++) {
        /* about 1.6 texels per pixel: a quarter turn spans the 90 degree view */
        int i = (off + (x * 128) / 320) & 511;
        int h1 = mount1[i], h2 = mount2[(i * 2) & 511];
        for (int y = HZ - h1; y < HZ; y++) fb[y * 320 + x] = far_c;
        for (int y = HZ - h2; y < HZ; y++) fb[y * 320 + x] = near_c;
    }
}

static void draw_sprite(color_t *fb, const color_t *spr, int sw, int sh, int ax, int ay, float sx, float sy, float scale)
{
    int w = (int)(sw * scale), h = (int)(sh * scale);
    if (w < 1 || h < 1) return;
    int x0 = (int)(sx - ax * scale), y0 = (int)(sy - ay * scale);
    int step = (int)(65536.0f / scale);
    int xa = x0 < 0 ? -x0 : 0, xb = x0 + w > 320 ? 320 - x0 : w;
    int ya = y0 < 0 ? -y0 : 0, yb = y0 + h > VIEW_H ? VIEW_H - y0 : h;
    for (int y = ya; y < yb; y++) {
        const color_t *row = spr + ((y * step) >> 16) * sw;
        color_t *d = fb + (y0 + y) * 320 + x0;
        int u = xa * step;
        for (int x = xa; x < xb; x++, u += step) {
            color_t c = row[u >> 16];
            if (c != KEYC) d[x] = c;
        }
    }
}

static void draw_shadow(color_t *fb, float sx, float sy, float scale)
{
    int rx = (int)(3.4f * scale), ry = (int)(1.2f * scale) + 1;
    int cx = (int)sx, cy = (int)sy;
    for (int y = -ry; y <= ry; y++) {
        int Y = cy + y;
        if (Y < HZ || Y >= VIEW_H) continue;
        int half = (int)(rx * sqrtf(1.0f - (float)(y * y) / (float)(ry * ry + 1)));
        for (int x = cx - half; x <= cx + half; x++)
            if ((unsigned)x < 320) fb[Y * 320 + x] = (fb[Y * 320 + x] >> 1) & 0x7BEF;
    }
}

static void render(void)
{
    color_t *fb = gfx_framebuffer();
    const Quality *q = &qual[quality];
    float ca = cosf(cam_a), sa = sinf(cam_a);
    float shx = shake > 0 ? frand(-shake, shake) : 0;
    float cx = cam_x + shx * 0.1f, cy = cam_y;

    draw_backdrop(fb, cam_a);
    color_t haze = fogpal[0][0];
    for (int y = HZ; y < VIEW_H; y++) {
        float d = CAM_H * FOC / (y - HZ + 0.5f);
        color_t *p = fb + y * 320;
        if (d > q->far) { for (int x = 0; x < 320; x++) p[x] = haze; continue; }
        float f = d / q->far;
        int lvl = (int)(32.5f - 32.0f * f * f);
        if (lvl < 0) lvl = 0;
        float wx = cx + ca * d, wy = cy + sa * d;            /* centre of the row */
        float rx = -sa * d / FOC, ry = ca * d / FOC;         /* one pixel to the right */
        int u = (int)((wx - rx * 160) * 65536.0f), v = (int)((wy - ry * 160) * 65536.0f);
        const color_t *pal = fogpal[lvl];
        if (q->cols == 160) m7_row2(p, p + 320, u, v, (int)(rx * 2 * 65536.0f), (int)(ry * 2 * 65536.0f), pal);
        else if (q->ss && d > 30) m7_row_ss(p, p + 320, u, v, (int)(rx * 65536.0f), (int)(ry * 65536.0f), pal);
        else m7_row(p, p + 320, u, v, (int)(rx * 65536.0f), (int)(ry * 65536.0f), pal);
    }

    /* sprites: karts and the nearest trees, sorted far to near */
    int n = 0;
    for (int i = 0; i < NKARTS; i++) {
        Kart *k = &karts[i];
        float dx = k->x - cx, dy = k->y - cy;
        float depth = dx * ca + dy * sa, lat = -dx * sa + dy * ca;
        if (depth < 3 || depth > q->far) continue;
        Draw *d = &draws[n++];
        d->depth = depth;
        d->scale = FOC / depth;
        d->sx = 160 + lat * d->scale;
        d->sy = HZ + CAM_H * d->scale;
        d->kind = 0;
        d->n = i;
    }
    int ntrees = 0;
    for (int i = 0; i < NTREES && ntrees < q->trees; i++) {
        float dx = trees[i].x - cx, dy = trees[i].y - cy;
        float depth = dx * ca + dy * sa, lat = -dx * sa + dy * ca;
        if (depth < 3 || depth > q->far * 0.8f || fabsf(lat) > depth * 1.2f + 20) continue;
        Draw *d = &draws[n++];
        d->depth = depth;
        d->scale = FOC / depth;
        d->sx = 160 + lat * d->scale;
        d->sy = HZ + CAM_H * d->scale;
        d->kind = 1;
        d->n = i;
        ntrees++;
    }
    for (int a = 1; a < n; a++) {                     /* insertion sort, far first */
        Draw t = draws[a];
        int b = a - 1;
        while (b >= 0 && draws[b].depth < t.depth) { draws[b + 1] = draws[b]; b--; }
        draws[b + 1] = t;
    }
    for (int i = 0; i < n; i++) {
        Draw *d = &draws[i];
        if (d->kind == 1) {
            draw_shadow(fb, d->sx + 2 * d->scale, d->sy, d->scale * 1.5f);
            draw_sprite(fb, tspr, TSPR_W, TSPR_H, TSPR_W / 2, TSPR_H - 1, d->sx, d->sy, d->scale / TPPU);
        } else {
            Kart *k = &karts[d->n];
            float bearing = atan2f(k->y - cy, k->x - cx);
            float rel = wrap_angle(k->a - bearing);
            if (d->n == 0 && drifting) rel += kbd_down(KEY_RIGHT) ? 0.4f : -0.4f;
            int ang = ((int)floorf(rel * NANG / (2 * PI) + 0.5f) % NANG + NANG) % NANG;
            draw_shadow(fb, d->sx, d->sy, d->scale);
            float hop = d->n == 0 && karts[0].bump > 0 ? sinf(karts[0].bump * 10) * 2 : 0;
            draw_sprite(fb, kspr + (d->n * NANG + ang) * KSPR_W * KSPR_H, KSPR_W, KSPR_H, KSPR_AX, KSPR_AY,
                        d->sx, d->sy - hop, d->scale / KPPU);
        }
    }
}

static void center_text(int y, const char *s, color_t c)
{
    int x = 160 - gfx_text_width(s) / 2;
    gfx_text(x + 1, y + 1, s, BLACK);
    gfx_text(x, y, s, c);
}

static const char *ordinal(int n) { return n == 1 ? "st" : n == 2 ? "nd" : n == 3 ? "rd" : "th"; }

static void fmt_time(char *b, int n, float t)
{
    int ms = (int)(t * 1000);
    snprintf(b, n, "%d:%02d.%02d", ms / 60000, ms / 1000 % 60, ms / 10 % 100);
}

static void draw_overlay(void)
{
    char buf[48], tb[16];
    if (state == ST_TITLE) {
        gfx_font(FONT_7X13);
        center_text(24, "M O D E - 7   K A R T", WHITE);
        gfx_font(FONT_6X10);
        center_text(44, "three laps, five rivals", LIGHTGRAY);
        if ((sys_millis() / 400) & 1) center_text(110, "press ENTER", YELLOW);
        if (best_race > 0) { fmt_time(tb, sizeof tb, best_race); snprintf(buf, sizeof buf, "best race %s", tb); center_text(90, buf, CYAN); }
        center_text(200, "UP gas  DOWN brake  LEFT/RIGHT steer  z drift", WHITE);
        center_text(214, "q quality  m mute  ESC quit", WHITE);
        return;
    }
    gfx_font(FONT_7X13);
    snprintf(buf, sizeof buf, "%d%s", player_place, ordinal(player_place));
    gfx_text(9, 7, buf, BLACK);
    gfx_text(8, 6, buf, player_place == 1 ? YELLOW : WHITE);
    int lap = karts[0].lap < 1 ? 1 : karts[0].lap > LAPS ? LAPS : karts[0].lap;
    snprintf(buf, sizeof buf, "LAP %d/%d", lap, LAPS);
    gfx_text(320 - 8 - gfx_text_width(buf), 6, buf, WHITE);
    gfx_font(FONT_6X10);
    if (state == ST_COUNT) {
        static const color_t light[3] = { RED, ORANGE, YELLOW };
        int lit = count_t / 60;
        for (int i = 0; i < 3; i++) {
            gfx_fill_circle(130 + i * 30, 60, 10, BLACK);
            gfx_fill_circle(130 + i * 30, 60, 8, i <= lit ? light[i] : DARKGRAY);
        }
    } else if (state == ST_RACE && race_time < 1.0f) {
        gfx_font(FONT_7X13);
        center_text(54, "G O !", GREEN);
        gfx_font(FONT_6X10);
    }
    if (state == ST_RACE && karts[0].lap > 1 && race_time - lap_start < 2.5f) {
        fmt_time(tb, sizeof tb, last_lap);
        snprintf(buf, sizeof buf, "lap %s%s", tb, last_lap == best_lap ? "  best!" : "");
        center_text(40, buf, CYAN);
    }
    if (boost_t > 0 && (sys_millis() / 100) & 1) center_text(220, "BOOST", ORANGE);
    if (state == ST_DONE) {
        gfx_fill_rect(70, 50, 180, 104, RGB(10, 14, 30));
        gfx_rect(70, 50, 180, 104, WHITE);
        gfx_font(FONT_7X13);
        snprintf(buf, sizeof buf, "%d%s PLACE", player_place, ordinal(player_place));
        center_text(58, buf, player_place == 1 ? YELLOW : WHITE);
        gfx_font(FONT_6X10);
        fmt_time(tb, sizeof tb, karts[0].finish_time);
        snprintf(buf, sizeof buf, "time %s", tb);
        center_text(80, buf, WHITE);
        fmt_time(tb, sizeof tb, best_lap);
        snprintf(buf, sizeof buf, "best lap %s", tb);
        center_text(94, buf, LIGHTGRAY);
        if (new_best) center_text(110, "new record!", YELLOW);
        if (count_t > 90) center_text(134, "ENTER to continue", LIGHTGRAY);
    }
}

static void draw_hud(void)
{
    char tb[16];
    gfx_fill_rect(0, HUD_Y, 320, 320 - HUD_Y, RGB(16, 20, 28));
    gfx_line(0, HUD_Y, 319, HUD_Y, RGB(70, 80, 100));
    int mx = 2, my = HUD_Y + 2;
    gfx_blit(mx, my, MINI, MINI, minimap);
    for (int i = NKARTS - 1; i >= 0; i--) {
        int x = mx + (int)karts[i].x * MINI / TEX_N, y = my + (int)karts[i].y * MINI / TEX_N;
        gfx_fill_rect(x - 1, y - 1, 3, 3, i == 0 ? WHITE : karts[i].body);
    }
    int tx = 86, ty = HUD_Y + 5;
    Kart *p = &karts[0];
    if (state == ST_TITLE) {
        gfx_text(tx, ty, "attract mode", GRAY);
        if (best_race > 0) { fmt_time(tb, sizeof tb, best_race); gfx_printf(tx, ty + 12, LIGHTGRAY, "REC %s", tb); }
    } else {
        gfx_printf(tx, ty, WHITE, "POS %d/%d", player_place, NKARTS);
        fmt_time(tb, sizeof tb, state == ST_DONE ? p->finish_time : race_time);
        gfx_printf(tx, ty + 12, WHITE, "T %s", tb);
        if (best_lap > 0) { fmt_time(tb, sizeof tb, best_lap); gfx_printf(tx, ty + 24, LIGHTGRAY, "B %s", tb); }
    }
    int kmh = (int)(fabsf(p->v) * 2.2f);
    gfx_printf(tx, ty + 38, LIGHTGRAY, "%3d km/h", kmh);
    int bar = kmh * 96 / 220;
    gfx_fill_rect(tx, ty + 52, 100, 8, RGB(40, 44, 56));
    gfx_fill_rect(tx + 2, ty + 54, bar > 96 ? 96 : bar, 4, boost_t > 0 ? ORANGE : offroad ? RGB(150, 120, 60) : GREEN);

    int rx = 196;
    const Quality *q = &qual[quality];
    gfx_printf(rx, ty, fps >= 25 ? GREEN : fps >= 15 ? YELLOW : RED, "FPS %5.1f", (double)fps);
    gfx_printf(rx, ty + 12, LIGHTGRAY, "3D  %5.1f ms", (double)ms_render);
    gfx_printf(rx, ty + 24, LIGHTGRAY, "LCD %5.1f ms", (double)ms_lcd);
    gfx_printf(rx, ty + 36, CYAN, "Q %s %d%s", q->name, q->cols, q->ss ? " x2" : "");
    gfx_printf(rx, ty + 48, GRAY, "far %d trees %d", (int)q->far, q->trees);
    gfx_printf(rx, ty + 60, GRAY, "heap %uK", (unsigned)(sys_free_heap() / 1024));
}

/* ============================================================ main */

void setup(void)
{
    gfx_clear(BLACK);
    gfx_font(FONT_7X13);
    center_text(150, "building the track...", WHITE);
    gfx_font(FONT_6X10);
    gfx_flush();
    track = malloc(TEX_N * TEX_N);
    kspr = malloc(NKARTS * NANG * KSPR_W * KSPR_H * sizeof(color_t));
    if (!track || !kspr) {
        gfx_clear(BLACK);
        gfx_text(10, 10, "not enough memory", RED);
        gfx_flush();
        sys_delay(2000);
        sys_exit();
    }
    FILE *f = fopen(HISCORE_FILE, "r");
    if (f) {
        int a, b;
        if (fscanf(f, "%d %d", &a, &b) == 2) best_race = a / 1000.0f;
        fclose(f);
    }
    sys_seed(sys_millis() ^ 0xCA7);
    build_centre_line();
    build_track();
    build_palettes();
    build_tree();
    build_karts();
    place_grid();
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
        if (k == 'm') muted = !muted;
        if (k == KEY_ENTER && state == ST_TITLE) new_race();
        else if (k == KEY_ENTER && state == ST_DONE && count_t > 90) { state = ST_TITLE; place_grid(); }
    }

    uint32_t now = sys_millis();
    int steps = 0;
    while (now - last_ms >= STEP_MS && steps < 4) { step(); last_ms += STEP_MS; steps++; }
    if (now - last_ms > STEP_MS * 4) last_ms = now;
    pump_audio(now - t0);

    uint64_t a = sys_micros();
    render();
    draw_overlay();
    uint64_t b = sys_micros();
    gfx_flush_rect(0, 0, 320, VIEW_H);
    uint64_t c = sys_micros();
    ms_render += ((b - a) / 1000.0f - ms_render) * 0.1f;
    ms_lcd += ((c - b) / 1000.0f - ms_lcd) * 0.1f;
    static uint64_t prev;
    if (prev) fps += (1e6f / (float)(c - prev) - fps) * 0.1f;
    prev = c;
    if (now - hud_ms >= 250) {
        hud_ms = now;
        draw_hud();
        gfx_flush_rect(0, HUD_Y, 320, 320 - HUD_Y);
    }
}
