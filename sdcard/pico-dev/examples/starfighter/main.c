/* starfighter - a polygon space shooter: fly through an asteroid field and fight waves of fighters.
 * Everything in the view is triangles: a hand built ship, two enemy types and icosphere asteroids
 * with three levels of detail, flat shaded or (HIGH and ULTRA) smooth shaded. The triangles of all
 * objects are bucket sorted by depth and drawn back to front by a scanline rasterizer with 32 shade
 * levels per material, fading into the dark. Explosions throw real triangle debris and sparks.
 * The bottom strip shows a radar, the score and where the frame time goes for each quality level.
 *
 *   arrows fly  SPACE/x fire  z barrel roll  i invert  q quality  m mute  ENTER start  ESC quit */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_H 240
#define HUD_Y VIEW_H
#define HC 120.0f                     /* screen row of the view axis */
#define FOCAL 220.0f
#define NEAR 4.0f
#define PZ 32.0f                      /* the ship flies this far in front of the camera */
#define SPEED 90.0f                   /* how fast the world streams past */
#define STEP_MS 16
#define DT (STEP_MS / 1000.0f)
#define PI 3.14159265f
#define BOX_X 55.0f
#define BOX_Y 38.0f
#define MAXV 200
#define MAX_TRIS 9000
#define NBUCKET 512
#define MAX_ROCKS 48
#define MAX_ENEMIES 12
#define MAX_BOLTS 40
#define MAX_LASERS 24
#define MAX_DEBRIS 200
#define MAX_SPARKS 240
#define MAX_FLASH 8
#define MAX_PICKUPS 4
#define NSTARS 160
#define PL_W 120
#define PL_H 72
#define KEYC 0xF81F
#define HISCORE_FILE SD_ROOT "/pico-dev/starfighter.hi"

enum { M_WHITE, M_GREY, M_BLUE, M_CANOPY, M_ENGINE, M_RED, M_DARKRED, M_PURPLE, M_ROCK0, M_ROCK1, M_ROCK2,
       M_HULL, M_HULL2, M_PICKUP, M_FIRE, NMAT };

static const uint8_t mat_rgb[NMAT][3] = {
    { 235, 235, 240 }, { 140, 145, 160 }, { 70, 130, 255 }, { 90, 200, 255 }, { 255, 150, 40 },
    { 220, 40, 40 }, { 120, 20, 30 }, { 150, 60, 200 }, { 150, 130, 110 }, { 120, 112, 104 }, { 170, 140, 115 },
    { 110, 140, 120 }, { 70, 90, 80 }, { 60, 255, 120 }, { 255, 200, 60 },
};
static const bool mat_glow[NMAT] = { false, false, false, false, true, false, false, false, false, false, false,
                                     false, false, true, true };

typedef struct { int lod, rocks; float far; bool smooth; int debris; const char *name; } Quality;

static const Quality qual[] = {
    { 0, 10, 600.0f, false, 6, "LOW" },
    { 1, 18, 900.0f, false, 10, "MED" },
    { 1, 28, 1200.0f, true, 14, "HIGH" },
    { 2, 40, 1500.0f, true, 20, "ULTRA" },
};
#define NQUAL 4

typedef struct { int nv, nf; float *v, *vn, *fn; uint16_t *f; uint8_t *fm; bool two_sided; } Mesh;
typedef struct { float x[3], y[3], l[3]; int mat, next; } Tri;
typedef struct { float x, y, z, vx, vy, vz, r, ax, ay, az, wx, wy, wz; int var, hp; bool alive; } Rock;
typedef struct { float x, y, z, hx, hy, hz, t, hold, fire_t, bank, seed; int type, hp, phase; bool alive, drop; } Enemy;
typedef struct { float x, y, z, vx, vy, vz, life; bool alive; } Shot;
typedef struct { float x, y, z, vx, vy, vz, a, da, life, o[9]; int mat; } Debris;
typedef struct { float x, y, z, vx, vy, vz, life; } Spark;
typedef struct { float x, y, z, r, life; } Flash;
typedef struct { float x, y, z, a; bool alive; } Pickup;
typedef struct { float x, y, z; } Star;

enum { ST_TITLE, ST_PLAY, ST_OVER };

static color_t mat_pal[NMAT][32];
static color_t bg_row[VIEW_H];
static color_t planet[PL_W * PL_H];
static Mesh ship_mesh, fighter_mesh, cruiser_mesh, pickup_mesh, rock_mesh[3][3];
static Tri *tris;
static int ntris, nverts, bucket[NBUCKET];
static float tvz[MAXV], tsx[MAXV], tsy[MAXV], tl[MAXV];
static color_t *fb_ptr;
static float vfar, bucket_k, ccr = 1, csr;

static Rock rocks[MAX_ROCKS];
static Enemy enemies[MAX_ENEMIES];
static Shot bolts[MAX_BOLTS], lasers[MAX_LASERS];
static Debris debris[MAX_DEBRIS];
static Spark sparks[MAX_SPARKS];
static Flash flashes[MAX_FLASH];
static Pickup pickups[MAX_PICKUPS];
static Star stars[NSTARS];
static int ndebris, nsparks;

static int state = ST_TITLE, quality = 1, wave, kills, score, best;
static float px, py, pvx, pvy, roll_t, invuln, shields, hit_flash, fire_cd, dead_t, wave_pause, banner_t, shake, game_t;
static float cam_x, cam_y = 9, cam_roll;
static bool muted, invert, new_best, ship_alive = true;
static uint32_t last_ms, t0, hud_ms;
static float fps, ms_render, ms_lcd;

static float frand(float a, float b) { return a + (b - a) * (float)(sys_random() & 0xFFFF) / 65535.0f; }
static int iceil(float f) { int i = (int)f; return f > (float)i ? i + 1 : i; }

static void norm3(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* R = Ry(yaw) * Rx(pitch) * Rz(roll), row major */
static void rot_matrix(float *R, float yaw, float pitch, float roll)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    R[0] = cy * cr + sy * sp * sr; R[1] = -cy * sr + sy * sp * cr; R[2] = sy * cp;
    R[3] = cp * sr;                R[4] = cp * cr;                 R[5] = -sp;
    R[6] = -sy * cr + cy * sp * sr; R[7] = sy * sr + cy * sp * cr; R[8] = cy * cp;
}

/* ============================================================ meshes */

static void mesh_alloc(Mesh *m, int maxv, int maxf, bool two_sided)
{
    m->nv = m->nf = 0;
    m->v = malloc(maxv * 3 * sizeof(float));
    m->vn = malloc(maxv * 3 * sizeof(float));
    m->fn = malloc(maxf * 3 * sizeof(float));
    m->f = malloc(maxf * 3 * sizeof(uint16_t));
    m->fm = malloc(maxf);
    m->two_sided = two_sided;
}

static int mesh_vert(Mesh *m, float x, float y, float z)
{
    float *p = m->v + m->nv * 3;
    p[0] = x; p[1] = y; p[2] = z;
    return m->nv++;
}

static void mesh_face(Mesh *m, int a, int b, int c, int mat)
{
    uint16_t *p = m->f + m->nf * 3;
    p[0] = (uint16_t)a; p[1] = (uint16_t)b; p[2] = (uint16_t)c;
    m->fm[m->nf++] = (uint8_t)mat;
}

/* face and vertex normals; outward flips faces that point at the model's origin */
static void mesh_finish(Mesh *m, bool outward)
{
    memset(m->vn, 0, m->nv * 3 * sizeof(float));
    for (int i = 0; i < m->nf; i++) {
        uint16_t *f = m->f + i * 3;
        float *a = m->v + f[0] * 3, *b = m->v + f[1] * 3, *c = m->v + f[2] * 3;
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
        float *n = m->fn + i * 3;
        n[0] = uy * wz - uz * wy; n[1] = uz * wx - ux * wz; n[2] = ux * wy - uy * wx;
        if (outward && (a[0] + b[0] + c[0]) * n[0] + (a[1] + b[1] + c[1]) * n[1] + (a[2] + b[2] + c[2]) * n[2] < 0) {
            uint16_t t = f[1]; f[1] = f[2]; f[2] = t;
            n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
        }
        norm3(n);
        for (int k = 0; k < 3; k++) {
            float *vn = m->vn + f[k] * 3;
            vn[0] += n[0]; vn[1] += n[1]; vn[2] += n[2];
        }
    }
    for (int i = 0; i < m->nv; i++) norm3(m->vn + i * 3);
}

static const float ship_v[][3] = {
    { 0.0f, 0.0f, 7.0f }, { 0.0f, 1.6f, 0.0f }, { -1.5f, 0.0f, -1.0f }, { 1.5f, 0.0f, -1.0f }, { 0.0f, -1.1f, -1.0f },
    { 0.0f, 0.6f, -4.0f }, { -7.0f, 1.0f, -4.5f }, { 7.0f, 1.0f, -4.5f }, { -3.6f, 3.0f, -5.2f }, { 3.6f, 3.0f, -5.2f },
    { -1.4f, 0.0f, -4.0f }, { 1.4f, 0.0f, -4.0f }, { -1.2f, -0.1f, 2.5f }, { 1.2f, -0.1f, 2.5f }, { 0.0f, -0.9f, -3.6f },
    { -3.0f, 0.45f, -2.6f }, { 3.0f, 0.45f, -2.6f }, { -3.6f, 0.6f, -4.3f }, { 3.6f, 0.6f, -4.3f },
};
static const uint8_t ship_f[][4] = {
    { 0, 2, 1, M_WHITE }, { 0, 1, 3, M_WHITE }, { 0, 4, 2, M_GREY }, { 0, 3, 4, M_GREY }, { 1, 2, 5, M_CANOPY },
    { 1, 5, 3, M_CANOPY }, { 2, 4, 14, M_GREY }, { 3, 14, 4, M_GREY }, { 2, 10, 5, M_GREY }, { 3, 5, 11, M_GREY },
    { 12, 6, 10, M_WHITE }, { 13, 11, 7, M_WHITE }, { 15, 8, 17, M_BLUE }, { 16, 18, 9, M_BLUE }, { 10, 5, 11, M_ENGINE },
    { 10, 11, 14, M_ENGINE }, { 2, 14, 10, M_GREY }, { 3, 11, 14, M_GREY },
};
static const float fighter_v[][3] = {
    { 0.0f, 0.0f, -6.0f }, { 0.0f, 2.0f, 1.0f }, { 0.0f, -2.0f, 1.0f }, { -2.5f, 0.0f, 1.0f }, { 2.5f, 0.0f, 1.0f },
    { 0.0f, 0.0f, 3.0f }, { -7.0f, 2.5f, 3.0f }, { -7.0f, -2.5f, 3.0f }, { 7.0f, 2.5f, 3.0f }, { 7.0f, -2.5f, 3.0f },
};
static const uint8_t fighter_f[][4] = {
    { 0, 1, 3, M_RED }, { 0, 4, 1, M_RED }, { 0, 3, 2, M_DARKRED }, { 0, 2, 4, M_DARKRED }, { 5, 3, 1, M_DARKRED },
    { 5, 1, 4, M_DARKRED }, { 5, 2, 3, M_ENGINE }, { 5, 4, 2, M_ENGINE }, { 3, 6, 7, M_PURPLE }, { 4, 9, 8, M_PURPLE },
};

static void mesh_from_table(Mesh *m, const float (*v)[3], int nv, const uint8_t (*f)[4], int nf)
{
    mesh_alloc(m, nv, nf, true);
    for (int i = 0; i < nv; i++) mesh_vert(m, v[i][0], v[i][1], v[i][2]);
    for (int i = 0; i < nf; i++) mesh_face(m, f[i][0], f[i][1], f[i][2], f[i][3]);
    mesh_finish(m, false);
}

static void add_octa(Mesh *m, float cx, float cy, float cz, float rx, float ry, float rz, int mat_front, int mat_back)
{
    static const uint8_t of[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
    int b = m->nv;
    mesh_vert(m, cx + rx, cy, cz); mesh_vert(m, cx - rx, cy, cz);
    mesh_vert(m, cx, cy + ry, cz); mesh_vert(m, cx, cy - ry, cz);
    mesh_vert(m, cx, cy, cz + rz); mesh_vert(m, cx, cy, cz - rz);
    for (int i = 0; i < 8; i++) mesh_face(m, b + of[i][0], b + of[i][1], b + of[i][2], i < 4 ? mat_back : mat_front);
}

static int midpoint(Mesh *m, int a, int b)
{
    float x = m->v[a * 3] + m->v[b * 3], y = m->v[a * 3 + 1] + m->v[b * 3 + 1], z = m->v[a * 3 + 2] + m->v[b * 3 + 2];
    float l = sqrtf(x * x + y * y + z * z);
    x /= l; y /= l; z /= l;
    for (int i = 0; i < m->nv; i++) {
        float *p = m->v + i * 3;
        if (fabsf(p[0] - x) + fabsf(p[1] - y) + fabsf(p[2] - z) < 1e-4f) return i;
    }
    return mesh_vert(m, x, y, z);
}

/* icosphere with 20 * 4^lod faces, pushed in and out by a bumpy function of the direction that
 * only depends on the variant, so the three levels of detail of a rock have the same shape */
static void rock_build(Mesh *m, int lod, int variant)
{
    static const uint8_t ifc[20][3] = {
        { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
        { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
    };
    static const float iv[12][3] = {
        { -1.0f, 1.618034f, 0.0f }, { 1.0f, 1.618034f, 0.0f }, { -1.0f, -1.618034f, 0.0f }, { 1.0f, -1.618034f, 0.0f },
        { 0.0f, -1.0f, 1.618034f }, { 0.0f, 1.0f, 1.618034f }, { 0.0f, -1.0f, -1.618034f }, { 0.0f, 1.0f, -1.618034f },
        { 1.618034f, 0.0f, -1.0f }, { 1.618034f, 0.0f, 1.0f }, { -1.618034f, 0.0f, -1.0f }, { -1.618034f, 0.0f, 1.0f },
    };
    static uint16_t tmp[320 * 3];
    mesh_alloc(m, 162, 320, false);
    for (int i = 0; i < 12; i++) {
        float p[3] = { iv[i][0], iv[i][1], iv[i][2] };
        norm3(p);
        mesh_vert(m, p[0], p[1], p[2]);
    }
    for (int i = 0; i < 20; i++) mesh_face(m, ifc[i][0], ifc[i][1], ifc[i][2], M_ROCK0 + variant);
    for (int s = 0; s < lod; s++) {
        int nf = m->nf;
        memcpy(tmp, m->f, nf * 3 * sizeof(uint16_t));
        m->nf = 0;
        for (int i = 0; i < nf; i++) {
            int a = tmp[i * 3], b = tmp[i * 3 + 1], c = tmp[i * 3 + 2];
            int ab = midpoint(m, a, b), bc = midpoint(m, b, c), ca = midpoint(m, c, a);
            mesh_face(m, a, ab, ca, M_ROCK0 + variant);
            mesh_face(m, b, bc, ab, M_ROCK0 + variant);
            mesh_face(m, c, ca, bc, M_ROCK0 + variant);
            mesh_face(m, ab, bc, ca, M_ROCK0 + variant);
        }
    }
    uint32_t seed = 1 + variant * 7919u;
    float dir[5][3], amp[5], freq[5], ph[5];
    for (int k = 0; k < 5; k++) {
        for (int j = 0; j < 3; j++) { seed = seed * 1664525u + 1013904223u; dir[k][j] = (float)(seed >> 8) / 8388608.0f - 1; }
        norm3(dir[k]);
        seed = seed * 1664525u + 1013904223u;
        ph[k] = (float)(seed >> 8) / 16777216.0f * 6.28f;
        amp[k] = 0.16f / (k + 1) + 0.04f;
        freq[k] = 1.5f + k * 1.3f;
    }
    for (int i = 0; i < m->nv; i++) {
        float *p = m->v + i * 3, r = 1.0f;
        for (int k = 0; k < 5; k++) r += amp[k] * sinf((p[0] * dir[k][0] + p[1] * dir[k][1] + p[2] * dir[k][2]) * freq[k] + ph[k]);
        p[0] *= r; p[1] *= r * (0.8f + 0.1f * variant); p[2] *= r;
    }
    mesh_finish(m, true);
}

static void build_meshes(void)
{
    mesh_from_table(&ship_mesh, ship_v, 19, ship_f, 18);
    mesh_from_table(&fighter_mesh, fighter_v, 10, fighter_f, 10);
    mesh_alloc(&cruiser_mesh, 18, 24, false);
    add_octa(&cruiser_mesh, 0, 0, 0, 4, 3, 11, M_HULL, M_HULL2);
    add_octa(&cruiser_mesh, -7, -1, 2, 2.2f, 2.2f, 6, M_HULL, M_ENGINE);
    add_octa(&cruiser_mesh, 7, -1, 2, 2.2f, 2.2f, 6, M_HULL, M_ENGINE);
    mesh_finish(&cruiser_mesh, false);
    mesh_alloc(&pickup_mesh, 6, 8, false);
    add_octa(&pickup_mesh, 0, 0, 0, 2.5f, 3.5f, 2.5f, M_PICKUP, M_PICKUP);
    mesh_finish(&pickup_mesh, false);
    for (int v = 0; v < 3; v++)
        for (int l = 0; l < 3; l++) rock_build(&rock_mesh[v][l], l, v);
}

static color_t blend565(color_t a, color_t b) { return (color_t)(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1)); }

static void build_palettes(void)
{
    for (int m = 0; m < NMAT; m++)
        for (int l = 0; l < 32; l++) {
            float k = l / 31.0f, hi = k > 0.85f ? (k - 0.85f) / 0.15f * 0.35f : 0;
            int c[3];
            for (int j = 0; j < 3; j++) {
                float v = mat_rgb[m][j] * k + (255 - mat_rgb[m][j]) * hi;
                c[j] = v > 255 ? 255 : (int)v;
            }
            mat_pal[m][l] = RGB(c[0], c[1], c[2]);
        }
    for (int y = 0; y < VIEW_H; y++) {
        float t = (float)y / VIEW_H;
        bg_row[y] = RGB((int)(2 + 10 * t), (int)(3 + 6 * t), (int)(14 + 20 * t));
    }
    /* a ringed gas giant, lit from the upper left */
    for (int y = 0; y < PL_H; y++)
        for (int x = 0; x < PL_W; x++) {
            float dx = x + 0.5f - PL_W / 2, dy = y + 0.5f - PL_H / 2, r = 26;
            float d2 = dx * dx + dy * dy;
            float e = (dx / 58) * (dx / 58) + (dy / 13) * (dy / 13), ei = (dx / 36) * (dx / 36) + (dy / 8) * (dy / 8);
            bool ring = e < 1 && ei > 1, body = d2 < r * r;
            color_t c = KEYC;
            if (ring && (!body || dy > 0)) {
                float k = 0.5f + 0.3f * sinf(e * 30);
                c = RGB((int)(190 * k), (int)(170 * k), (int)(130 * k));
            } else if (body) {
                float nz = sqrtf(1 - d2 / (r * r));
                float d = (-dx * 0.55f - dy * 0.45f) / r + nz * 0.6f, lit = 0.15f + 0.85f * (d > 0 ? d : 0);
                if (lit > 1) lit = 1;
                float band = 0.75f + 0.25f * sinf(dy / r * 9 + sinf(dx * 0.12f) * 0.6f);
                c = RGB((int)(230 * lit * band), (int)(150 * lit * band), (int)(90 * lit * band));
                if (c == KEYC) c ^= 0x20;
            }
            planet[y * PL_W + x] = c;
        }
}

/* ============================================================ rasterizer */

static void span_flat(color_t *p, int n, color_t c)
{
    if (((uintptr_t)p & 2) && n > 0) { *p++ = c; n--; }
    uint32_t *q = (uint32_t *)p, *e = q + (n >> 1), cc = c | (uint32_t)c << 16;
    while (q < e) *q++ = cc;
    if (n & 1) *(color_t *)q = c;
}

static void span_smooth(color_t *p, color_t *e, int l, int dl, const color_t *pal)
{
    for (; p < e; p++, l += dl) *p = pal[l >> 16];
}

static float clamp_level(float l) { return l < 0 ? 0 : l > 31.9f ? 31.9f : l; }

static void raster(const Tri *t)
{
    int i0 = 0, i1 = 1, i2 = 2, k;
    if (t->y[i1] < t->y[i0]) { k = i0; i0 = i1; i1 = k; }
    if (t->y[i2] < t->y[i0]) { k = i0; i0 = i2; i2 = k; }
    if (t->y[i2] < t->y[i1]) { k = i1; i1 = i2; i2 = k; }
    float x0 = t->x[i0], y0 = t->y[i0], x1 = t->x[i1], y1 = t->y[i1], x2 = t->x[i2], y2 = t->y[i2];
    float dy2 = y2 - y0, dx1 = x1 - x0, dy1 = y1 - y0, dx2 = x2 - x0;
    if (dy2 < 0.001f || y2 < 0 || y0 >= VIEW_H) return;
    float area = dx1 * dy2 - dx2 * dy1;
    if (area > -0.01f && area < 0.01f) return;
    float l0 = t->l[i0], dl1 = t->l[i1] - l0, dl2 = t->l[i2] - l0;
    bool flat = dl1 == 0 && dl2 == 0;
    float gx = (dl1 * dy2 - dl2 * dy1) / area, gy = (dx1 * dl2 - dx2 * dl1) / area;
    const color_t *pal = mat_pal[t->mat];
    color_t fc = pal[(int)clamp_level(l0)];
    float s02 = dx2 / dy2, s01 = dy1 > 0.001f ? dx1 / dy1 : 0, s12 = y2 - y1 > 0.001f ? (x2 - x1) / (y2 - y1) : 0;
    int ys = iceil(y0 < 0 ? 0 : y0), ym = iceil(y1 < 0 ? 0 : y1 > VIEW_H ? VIEW_H : y1), ye = iceil(y2 > VIEW_H ? VIEW_H : y2);
    bool long_left = area > 0;
    for (int y = ys; y < ye; y++) {
        float fy = (float)y;
        float xa = x0 + s02 * (fy - y0);
        float xb = y < ym ? x0 + s01 * (fy - y0) : x1 + s12 * (fy - y1);
        float xl = long_left ? xa : xb, xr = long_left ? xb : xa;
        if (xl < 0) xl = 0;
        if (xr > 320) xr = 320;
        int xs = iceil(xl), xe = iceil(xr);
        if (xs >= xe) continue;
        color_t *p = fb_ptr + y * 320 + xs;
        if (flat) { span_flat(p, xe - xs, fc); continue; }
        float ls = l0 + gx * (xs - x0) + gy * (fy - y0), le = ls + gx * (xe - 1 - xs);
        ls = clamp_level(ls);
        le = clamp_level(le);
        int n = xe - xs, li = (int)(ls * 65536.0f);
        span_smooth(p, p + n, li, n > 1 ? (int)((le - ls) * 65536.0f) / (n - 1) : 0, pal);
    }
}

static void push_tri(int a, int b, int c, float la, float lb, float lc, int mat, float z)
{
    if (ntris >= MAX_TRIS) return;
    Tri *t = &tris[ntris];
    t->x[0] = tsx[a]; t->x[1] = tsx[b]; t->x[2] = tsx[c];
    t->y[0] = tsy[a]; t->y[1] = tsy[b]; t->y[2] = tsy[c];
    t->l[0] = la; t->l[1] = lb; t->l[2] = lc;
    t->mat = mat;
    int k = (int)(z * bucket_k);
    if (k < 0) k = 0;
    if (k >= NBUCKET) k = NBUCKET - 1;
    t->next = bucket[k];
    bucket[k] = ntris++;
}

#define LX -0.45f
#define LY 0.75f
#define LZ -0.48f

static float light(float nx, float ny, float nz)
{
    float d = nx * LX + ny * LY + nz * LZ;
    return 0.2f + 0.8f * (d > 0 ? d : 0);
}

static float fog(float z)
{
    float f = z < vfar * 0.45f ? 1.0f : (vfar - z) / (vfar * 0.55f);
    return f < 0 ? 0 : f;
}

static bool project(float x, float y, float z, float *sx, float *sy)
{
    if (z < NEAR) return false;
    float rx = x - cam_x, ry = y - cam_y, k = FOCAL / z;
    *sx = 160 + (rx * ccr + ry * csr) * k;
    *sy = HC - (ry * ccr - rx * csr) * k;
    return true;
}

static void draw_mesh(const Mesh *m, const float *R, float s, float ox, float oy, float oz, float br, bool smooth)
{
    float rx = ox - cam_x, ry = oy - cam_y;
    if (oz + br < NEAR || oz - br > vfar) return;
    if (sqrtf(rx * rx + ry * ry) > oz * 0.95f + br * 1.4f) return;
    int nv = m->nv;
    nverts += nv;
    const float *v = m->v;
    for (int i = 0; i < nv; i++, v += 3) {
        float wx = (R[0] * v[0] + R[1] * v[1] + R[2] * v[2]) * s + rx;
        float wy = (R[3] * v[0] + R[4] * v[1] + R[5] * v[2]) * s + ry;
        float wz = (R[6] * v[0] + R[7] * v[1] + R[8] * v[2]) * s + oz;
        tvz[i] = wz;
        if (wz >= NEAR) {
            float k = FOCAL / wz;
            tsx[i] = 160 + (wx * ccr + wy * csr) * k;
            tsy[i] = HC - (wy * ccr - wx * csr) * k;
        }
    }
    if (smooth) {
        const float *n = m->vn;
        for (int i = 0; i < nv; i++, n += 3) {
            float nx = R[0] * n[0] + R[1] * n[1] + R[2] * n[2], ny = R[3] * n[0] + R[4] * n[1] + R[5] * n[2], nz = R[6] * n[0] + R[7] * n[1] + R[8] * n[2];
            tl[i] = 31.0f * light(nx, ny, nz) * fog(tvz[i]);
        }
    }
    const uint16_t *f = m->f;
    for (int i = 0; i < m->nf; i++, f += 3) {
        int a = f[0], b = f[1], c = f[2];
        if (tvz[a] < NEAR || tvz[b] < NEAR || tvz[c] < NEAR) continue;
        float cross = (tsx[b] - tsx[a]) * (tsy[c] - tsy[a]) - (tsx[c] - tsx[a]) * (tsy[b] - tsy[a]);
        bool front = cross > 0;
        if (!front && !m->two_sided) continue;
        float z = (tvz[a] + tvz[b] + tvz[c]) * 0.3333f;
        int mat = m->fm[i];
        if (smooth && !mat_glow[mat]) { push_tri(a, b, c, tl[a], tl[b], tl[c], mat, z); continue; }
        float lv;
        if (mat_glow[mat]) lv = 31.0f * fog(z);
        else {
            const float *n = m->fn + i * 3;
            float nx = R[0] * n[0] + R[1] * n[1] + R[2] * n[2], ny = R[3] * n[0] + R[4] * n[1] + R[5] * n[2], nz = R[6] * n[0] + R[7] * n[1] + R[8] * n[2];
            if (!front) { nx = -nx; ny = -ny; nz = -nz; }
            lv = 31.0f * light(nx, ny, nz) * fog(z);
        }
        push_tri(a, b, c, lv, lv, lv, mat, z);
    }
}

/* ============================================================ sound */

#define RATE 11025
#define LEAD_SAMPLES (RATE / 8)
#define BLOCK 256
static int16_t audio[BLOCK];
static uint32_t sent;
static float las_t, las_f, las_phase, boom_t, hit_t, hit_phase, beep_f, beep_t, beep_phase, lp, hum;

static void beep(float f, float len) { beep_f = f; beep_t = len; }
static float noise(void) { return (float)(int)(sys_random() & 0xFFF) / 2048.0f - 1; }

static void synth(int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        float s = 0;
        if (state == ST_PLAY && ship_alive) {
            hum += (noise() - hum) * 0.02f;
            s += hum * 0.5f;
        }
        if (las_t > 0) {
            las_phase += las_f / RATE;
            if (las_phase >= 1) las_phase -= 1;
            s += las_phase < 0.5f ? 0.09f : -0.09f;
            las_f *= 0.9993f;
            las_t -= 1.0f / RATE;
        }
        if (boom_t > 0) {
            lp += (noise() - lp) * 0.10f;
            s += lp * boom_t * 1.4f;
            boom_t -= 1.0f / RATE;
        }
        if (hit_t > 0) {
            hit_phase += 80.0f / RATE;
            if (hit_phase >= 1) hit_phase -= 1;
            s += (hit_phase < 0.5f ? 0.3f : -0.3f) * hit_t * 3;
            hit_t -= 1.0f / RATE;
        }
        if (beep_t > 0) {
            beep_phase += beep_f / RATE;
            if (beep_phase >= 1) beep_phase -= 1;
            s += beep_phase < 0.5f ? 0.15f : -0.15f;
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

/* ============================================================ game */

static void spawn_rock(Rock *k, float z)
{
    k->alive = true;
    k->r = frand(5, 16);
    if (sys_random() % 10 < 4) { k->x = frand(-BOX_X - 10, BOX_X + 10); k->y = frand(-BOX_Y - 10, BOX_Y + 10); }
    else { k->x = frand(-260, 260); k->y = frand(-190, 190); }
    k->z = z;
    k->vx = frand(-4, 4); k->vy = frand(-4, 4); k->vz = frand(-25, 5);
    k->ax = frand(0, 6.28f); k->ay = frand(0, 6.28f); k->az = frand(0, 6.28f);
    k->wx = frand(-1.5f, 1.5f); k->wy = frand(-1.5f, 1.5f); k->wz = frand(-1, 1);
    k->var = (int)(sys_random() % 3);
    k->hp = 1 + (int)(k->r / 5);
}

static void explode(float x, float y, float z, float size, int mat, int n)
{
    for (int i = 0; i < n && ndebris < MAX_DEBRIS; i++) {
        Debris *d = &debris[ndebris++];
        d->x = x + frand(-0.5f, 0.5f) * size; d->y = y + frand(-0.5f, 0.5f) * size; d->z = z + frand(-0.5f, 0.5f) * size;
        d->vx = frand(-3, 3) * size; d->vy = frand(-3, 3) * size; d->vz = frand(-3, 3) * size - SPEED * 0.3f;
        for (int j = 0; j < 9; j++) d->o[j] = frand(-0.4f, 0.4f) * size;
        d->a = frand(0, 6.28f); d->da = frand(-9, 9);
        d->life = frand(1.0f, 1.8f);
        d->mat = i % 3 == 0 ? M_FIRE : mat;
    }
    for (int i = 0; i < n * 2 && nsparks < MAX_SPARKS; i++) {
        Spark *s = &sparks[nsparks++];
        s->x = x; s->y = y; s->z = z;
        s->vx = frand(-8, 8) * size; s->vy = frand(-8, 8) * size; s->vz = frand(-8, 8) * size;
        s->life = frand(0.3f, 0.8f);
    }
    for (int i = 0; i < MAX_FLASH; i++)
        if (flashes[i].life <= 0) {
            flashes[i].x = x; flashes[i].y = y; flashes[i].z = z; flashes[i].r = size * 1.6f; flashes[i].life = 0.35f;
            break;
        }
    if (boom_t < 0.5f) boom_t = size > 8 ? 0.8f : 0.5f;
}

static void damage(int n)
{
    if (state != ST_PLAY || invuln > 0 || !ship_alive) return;
    shields -= n;
    invuln = 1.0f;
    hit_flash = 0.25f;
    shake = 5;
    hit_t = 0.25f;
    if (shields <= 0) {
        shields = 0;
        ship_alive = false;
        explode(px, py, PZ, 10, M_WHITE, 30);
        dead_t = 0;
        if (score > best) {
            best = score;
            new_best = true;
            FILE *f = fopen(HISCORE_FILE, "w");
            if (f) { fprintf(f, "%d\n", best); fclose(f); }
        }
    }
}

static void start_wave(void)
{
    wave++;
    int n = 3 + wave;
    if (n > MAX_ENEMIES) n = MAX_ENEMIES;
    int ncru = wave >= 3 ? 1 + (wave - 3) / 2 : 0;
    if (ncru > 3) ncru = 3;
    int drop = (int)(sys_random() % n);
    for (int i = 0; i < n; i++) {
        Enemy *e = &enemies[i];
        e->alive = true;
        e->type = i < ncru ? 1 : 0;
        e->hp = e->type ? 8 : 1;
        e->hx = (i - (n - 1) * 0.5f) * 20 + frand(-5, 5);
        e->hy = frand(-25, 25);
        e->hz = e->type ? frand(200, 240) : frand(110, 170);
        e->x = e->hx * 2.5f;
        e->y = e->hy + 50;
        e->z = vfar * 0.9f + i * 30;
        e->t = 0;
        e->hold = e->type ? 16 : frand(7, 10);
        e->fire_t = frand(1.5f, 3.5f);
        e->phase = 0;
        e->bank = 0;
        e->seed = frand(0, 6.28f);
        e->drop = e->type == 1 || i == drop;
    }
    banner_t = 2.0f;
    beep(660, 0.12f);
}

static void fire_bolt(const Enemy *e, float ox, float oy)
{
    for (int i = 0; i < MAX_BOLTS; i++) {
        Shot *b = &bolts[i];
        if (b->alive) continue;
        float dx = px + ox - e->x, dy = py + oy - e->y, dz = PZ - e->z;
        float l = sqrtf(dx * dx + dy * dy + dz * dz), sp = 110 + wave * 6.0f;
        b->alive = true;
        b->x = e->x; b->y = e->y; b->z = e->z - 4;
        b->vx = dx / l * sp; b->vy = dy / l * sp; b->vz = dz / l * sp;
        b->life = 5;
        return;
    }
}

static void spawn_pickup(float x, float y, float z)
{
    for (int i = 0; i < MAX_PICKUPS; i++)
        if (!pickups[i].alive) {
            pickups[i].alive = true;
            pickups[i].x = x; pickups[i].y = y; pickups[i].z = z; pickups[i].a = 0;
            return;
        }
}

static void kill_enemy(Enemy *e)
{
    e->alive = false;
    kills++;
    if (state == ST_PLAY) score += e->type ? 500 : 100;
    explode(e->x, e->y, e->z, e->type ? 12 : 6, e->type ? M_HULL : M_RED, e->type ? qual[quality].debris * 2 : qual[quality].debris);
    if (e->drop) spawn_pickup(e->x, e->y, e->z);
}

static void step_enemies(void)
{
    int alive = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;
        alive++;
        float ox = e->x;
        if (e->phase == 0) {
            e->z -= 170 * DT;
            e->x += (e->hx - e->x) * 1.2f * DT;
            e->y += (e->hy - e->y) * 1.2f * DT;
            if (e->z <= e->hz) { e->phase = 1; e->t = 0; }
        } else if (e->phase == 1) {
            e->t += DT;
            e->hx += (px - e->hx > 0 ? 8.0f : -8.0f) * DT;
            float tx = e->hx + sinf(e->t * 1.3f + e->seed) * 28, ty = e->hy + cosf(e->t * 0.9f + e->seed) * 14;
            e->x += (tx - e->x) * 2 * DT;
            e->y += (ty - e->y) * 2 * DT;
            e->z += (e->hz + sinf(e->t * 0.7f) * 12 - e->z) * 2 * DT;
            e->fire_t -= DT;
            if (e->fire_t <= 0 && ship_alive) {
                if (e->type) { fire_bolt(e, 0, 0); fire_bolt(e, -14, 0); fire_bolt(e, 14, 0); }
                else fire_bolt(e, 0, 0);
                e->fire_t = frand(1.2f, 2.6f) / (1 + wave * 0.12f);
            }
            if (e->t > e->hold) e->phase = 2;
        } else {
            e->z -= 150 * DT;
            e->x += (e->x > px ? 30.0f : -30.0f) * DT;
            e->y += 18 * DT;
            if (e->z < -20) e->alive = false;
        }
        e->bank += (-(e->x - ox) / DT * 0.02f - e->bank) * 4 * DT;
        float dx = e->x - px, dy = e->y - py, dz = e->z - PZ, r = e->type ? 11.0f : 6.0f;
        if (dx * dx + dy * dy + dz * dz < r * r) { damage(25); kill_enemy(e); }
    }
    if (alive == 0) {
        wave_pause -= DT;
        if (wave_pause <= 0) { start_wave(); wave_pause = 2.5f; }
    }
}

static void step_shots(void)
{
    for (int i = 0; i < MAX_LASERS; i++) {
        Shot *s = &lasers[i];
        if (!s->alive) continue;
        s->x += s->vx * DT; s->y += s->vy * DT; s->z += s->vz * DT;
        if (s->z > vfar) { s->alive = false; continue; }
        for (int j = 0; j < MAX_ROCKS && s->alive; j++) {
            Rock *k = &rocks[j];
            if (!k->alive) continue;
            float dx = k->x - s->x, dy = k->y - s->y, dz = k->z - s->z;
            if (dx * dx + dy * dy + dz * dz > (k->r + 2) * (k->r + 2)) continue;
            s->alive = false;
            for (int n = 0; n < 4 && nsparks < MAX_SPARKS; n++) {
                Spark *p = &sparks[nsparks++];
                p->x = s->x; p->y = s->y; p->z = s->z - 2;
                p->vx = frand(-30, 30); p->vy = frand(-30, 30); p->vz = frand(-60, 0);
                p->life = 0.3f;
            }
            if (--k->hp <= 0) {
                k->alive = false;
                if (state == ST_PLAY) score += 20 + (int)k->r * 3;
                explode(k->x, k->y, k->z, k->r * 0.8f, M_ROCK0 + k->var, qual[quality].debris);
            }
        }
        for (int j = 0; j < MAX_ENEMIES && s->alive; j++) {
            Enemy *e = &enemies[j];
            if (!e->alive) continue;
            float dx = e->x - s->x, dy = e->y - s->y, dz = e->z - s->z, r = e->type ? 12.0f : 8.0f;
            if (dx * dx + dy * dy + dz * dz > r * r) continue;
            s->alive = false;
            if (--e->hp <= 0) kill_enemy(e);
            else beep(220, 0.04f);
        }
    }
    for (int i = 0; i < MAX_BOLTS; i++) {
        Shot *b = &bolts[i];
        if (!b->alive) continue;
        b->x += b->vx * DT; b->y += b->vy * DT; b->z += b->vz * DT;
        b->life -= DT;
        if (b->z < PZ - 20 || b->life <= 0) { b->alive = false; continue; }
        float dx = b->x - px, dy = b->y - py, dz = b->z - PZ;
        if (dx * dx + dy * dy + dz * dz < 3.5f * 3.5f && ship_alive) {
            b->alive = false;
            if (roll_t > 0) beep(1500, 0.05f);    /* the barrel roll deflects it */
            else damage(10);
        }
    }
}

static void step_world(void)
{
    const Quality *q = &qual[quality];
    int want = q->rocks, n = 0;
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *k = &rocks[i];
        if (!k->alive) continue;
        n++;
        k->x += k->vx * DT; k->y += k->vy * DT; k->z += (k->vz - SPEED) * DT;
        k->ax += k->wx * DT; k->ay += k->wy * DT; k->az += k->wz * DT;
        if (k->z < -k->r - 10) { k->alive = false; continue; }
        float dx = k->x - px, dy = k->y - py, dz = k->z - PZ, r = k->r * 0.8f + 3.5f;
        if (ship_alive && dx * dx + dy * dy + dz * dz < r * r && state == ST_PLAY && invuln <= 0) {
            damage(20);
            k->alive = false;
            explode(k->x, k->y, k->z, k->r * 0.8f, M_ROCK0 + k->var, q->debris);
        }
    }
    for (int i = 0; i < MAX_ROCKS && n < want; i++)
        if (!rocks[i].alive) { spawn_rock(&rocks[i], vfar * frand(0.85f, 1.0f)); n++; }
    for (int i = MAX_ROCKS - 1; i >= 0 && n > want; i--)   /* quality dropped: thin the field out of sight */
        if (rocks[i].alive && rocks[i].z > vfar * 0.8f) { rocks[i].alive = false; n--; }
    for (int i = 0; i < NSTARS; i++) {
        Star *s = &stars[i];
        s->z -= SPEED * 2 * DT;
        if (s->z < 20) { s->z += 1500; s->x = frand(-900, 900); s->y = frand(-700, 700); }
    }
    for (int i = 0; i < MAX_PICKUPS; i++) {
        Pickup *p = &pickups[i];
        if (!p->alive) continue;
        p->z -= SPEED * 0.6f * DT;
        p->a += 3 * DT;
        p->x += (px - p->x) * 0.3f * DT;
        p->y += (py - p->y) * 0.3f * DT;
        if (p->z < -10) p->alive = false;
        float dx = p->x - px, dy = p->y - py, dz = p->z - PZ;
        if (ship_alive && dx * dx + dy * dy + dz * dz < 7 * 7) {
            p->alive = false;
            shields = shields + 30 > 100 ? 100 : shields + 30;
            if (state == ST_PLAY) score += 50;
            beep(1320, 0.2f);
        }
    }
    for (int i = 0; i < ndebris; i++) {
        Debris *d = &debris[i];
        d->x += d->vx * DT; d->y += d->vy * DT; d->z += (d->vz - SPEED * 0.4f) * DT;
        d->a += d->da * DT;
        d->life -= DT;
        if (d->life <= 0 || d->z < -10) { *d = debris[--ndebris]; i--; }
    }
    for (int i = 0; i < nsparks; i++) {
        Spark *s = &sparks[i];
        s->x += s->vx * DT; s->y += s->vy * DT; s->z += (s->vz - SPEED * 0.4f) * DT;
        s->life -= DT;
        if (s->life <= 0) { *s = sparks[--nsparks]; i--; }
    }
    for (int i = 0; i < MAX_FLASH; i++) if (flashes[i].life > 0) { flashes[i].life -= DT; flashes[i].z -= SPEED * 0.4f * DT; }
}

static void step_player(void)
{
    float ix = 0, iy = 0;
    bool fire = false;
    if (state == ST_PLAY && ship_alive) {
        ix = (kbd_down(KEY_RIGHT) ? 1.0f : 0.0f) - (kbd_down(KEY_LEFT) ? 1.0f : 0.0f);
        iy = (kbd_down(KEY_UP) ? 1.0f : 0.0f) - (kbd_down(KEY_DOWN) ? 1.0f : 0.0f);
        if (invert) iy = -iy;
        fire = kbd_down(KEY_SPACE) || kbd_down('x');
    } else if (state == ST_TITLE) {
        /* attract mode: weave about and shoot at whatever is ahead */
        float tx = sinf(game_t * 0.5f) * 40, ty = sinf(game_t * 0.8f) * 22;
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (enemies[i].alive && enemies[i].phase == 1) { tx = enemies[i].x; ty = enemies[i].y; break; }
        ix = tx - px > 3 ? 1.0f : tx - px < -3 ? -1.0f : 0.0f;
        iy = ty - py > 3 ? 1.0f : ty - py < -3 ? -1.0f : 0.0f;
        fire = (int)(game_t * 3) % 3 != 0;
    }
    pvx += ix * 420 * DT; pvy += iy * 340 * DT;
    pvx *= 0.9f; pvy *= 0.9f;
    px += pvx * DT; py += pvy * DT;
    if (px < -BOX_X) { px = -BOX_X; pvx = 0; }
    if (px > BOX_X) { px = BOX_X; pvx = 0; }
    if (py < -BOX_Y) { py = -BOX_Y; pvy = 0; }
    if (py > BOX_Y) { py = BOX_Y; pvy = 0; }
    if (fire_cd > 0) fire_cd -= DT;
    if (fire && fire_cd <= 0 && ship_alive) {
        fire_cd = 0.13f;
        int made = 0;
        for (int i = 0; i < MAX_LASERS && made < 2; i++) {
            Shot *s = &lasers[i];
            if (s->alive) continue;
            float side = made ? 4.0f : -4.0f;
            s->alive = true;
            s->x = px + side; s->y = py - 0.4f; s->z = PZ + 4;
            s->vx = -side * 500 / 200; s->vy = 0; s->vz = 500;   /* the two bolts meet at the crosshair */
            made++;
        }
        las_t = 0.07f; las_f = 1500;
    }
    if (roll_t > 0) roll_t -= DT;
    if (invuln > 0) invuln -= DT;
    if (hit_flash > 0) hit_flash -= DT;
    if (shake > 0) shake -= 0.3f;
    cam_x += (px * 0.8f - cam_x) * 0.2f;
    cam_y += (py * 0.8f + 9 - cam_y) * 0.2f;
    cam_roll += (-pvx * 0.012f * 0.35f - cam_roll) * 0.2f;
}

static void reset_world(void)
{
    memset(enemies, 0, sizeof enemies);
    memset(bolts, 0, sizeof bolts);
    memset(lasers, 0, sizeof lasers);
    memset(pickups, 0, sizeof pickups);
    ndebris = nsparks = 0;
    wave = kills = score = 0;
    wave_pause = 1.5f;
    shields = 100;
    ship_alive = true;
    invuln = roll_t = 0;
    px = py = pvx = pvy = 0;
    new_best = false;
}

static void step(void)
{
    game_t += DT;
    if (banner_t > 0) banner_t -= DT;
    step_player();
    step_world();
    step_enemies();
    step_shots();
    if (state == ST_PLAY && !ship_alive) {
        dead_t += DT;
        if (dead_t > 2.0f) state = ST_OVER;
    }
}

/* ============================================================ renderer */

static void plot(int x, int y, color_t c)
{
    if ((unsigned)x < 320 && (unsigned)y < VIEW_H) fb_ptr[y * 320 + x] = c;
}

static void line2(float x0, float y0, float x1, float y1, color_t c, color_t core)
{
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy)) + 1;
    if (n > 400) n = 400;
    float sx = dx / n, sy = dy / n;
    for (int i = 0; i <= n; i++, x0 += sx, y0 += sy) {
        int x = (int)x0, y = (int)y0;
        plot(x + 1, y, c); plot(x, y + 1, c); plot(x - 1, y, c); plot(x, y - 1, c);
        plot(x, y, core);
    }
}

static void disc(int cx, int cy, int r, color_t c, bool blend)
{
    if (r > 120) r = 120;
    for (int y = -r; y <= r; y++) {
        int Y = cy + y;
        if (Y < 0 || Y >= VIEW_H) continue;
        int h = (int)sqrtf((float)(r * r - y * y));
        int xa = cx - h < 0 ? 0 : cx - h, xb = cx + h > 319 ? 319 : cx + h;
        color_t *p = fb_ptr + Y * 320;
        for (int x = xa; x <= xb; x++) p[x] = blend ? blend565(p[x], c) : c;
    }
}

static void draw_backdrop(void)
{
    for (int y = 0; y < VIEW_H; y++) span_flat(fb_ptr + y * 320, 320, bg_row[y]);
    /* the planet is far away: it barely moves, but it turns with the camera roll */
    float X = 70 - cam_x * 0.12f, Y = 50 - cam_y * 0.12f;
    int cx = (int)(160 + X * ccr + Y * csr), cy = (int)(HC - (Y * ccr - X * csr));
    int x0 = cx - PL_W / 2, y0 = cy - PL_H / 2;
    for (int y = 0; y < PL_H; y++) {
        int Y2 = y0 + y;
        if ((unsigned)Y2 >= VIEW_H) continue;
        const color_t *s = planet + y * PL_W;
        color_t *d = fb_ptr + Y2 * 320;
        for (int x = 0; x < PL_W; x++) {
            int X2 = x0 + x;
            if ((unsigned)X2 < 320 && s[x] != KEYC) d[X2] = s[x];
        }
    }
    for (int i = 0; i < NSTARS; i++) {
        Star *s = &stars[i];
        float sx, sy;
        if (!project(s->x + cam_x * 0.7f, s->y + cam_y * 0.7f, s->z, &sx, &sy)) continue;
        int b = (int)(255 - s->z * 0.15f);
        if (b < 40) b = 40;
        color_t c = RGB(b, b, b > 200 ? 255 : b + 40);
        int x = (int)sx, y = (int)sy;
        plot(x, y, c);
        if (s->z < 300) { plot(x + 1, y, c); plot(x, y + 1, c); plot(x + 1, y + 1, c); }
    }
}

static void draw_debris(const Debris *d)
{
    float ca = cosf(d->a), sa = sinf(d->a), w[9];
    for (int k = 0; k < 3; k++) {             /* spin about the z then the x axis */
        const float *o = d->o + k * 3;
        float x = o[0] * ca - o[1] * sa, y = o[0] * sa + o[1] * ca, z = o[2];
        w[k * 3] = d->x + x;
        w[k * 3 + 1] = d->y + y * ca - z * sa;
        w[k * 3 + 2] = d->z + y * sa + z * ca;
    }
    int b = MAXV - 3;
    for (int k = 0; k < 3; k++) {
        if (!project(w[k * 3], w[k * 3 + 1], w[k * 3 + 2], &tsx[b + k], &tsy[b + k])) return;
    }
    float ux = w[3] - w[0], uy = w[4] - w[1], uz = w[5] - w[2], vx = w[6] - w[0], vy = w[7] - w[1], vz = w[8] - w[2];
    float n[3] = { uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
    norm3(n);
    float dd = fabsf(n[0] * LX + n[1] * LY + n[2] * LZ), z = (w[2] + w[5] + w[8]) * 0.3333f;
    float lv = (mat_glow[d->mat] ? 31.0f * (d->life > 1 ? 1 : d->life) : 31.0f * (0.25f + 0.75f * dd)) * fog(z);
    push_tri(b, b + 1, b + 2, lv, lv, lv, d->mat, z);
}

static void render(void)
{
    const Quality *q = &qual[quality];
    fb_ptr = gfx_framebuffer();
    vfar = q->far;
    bucket_k = NBUCKET / vfar;
    for (int i = 0; i < NBUCKET; i++) bucket[i] = -1;
    ntris = nverts = 0;
    float save_x = cam_x, save_y = cam_y;
    if (shake > 0) { cam_x += frand(-shake, shake) * 0.3f; cam_y += frand(-shake, shake) * 0.3f; }
    ccr = cosf(cam_roll); csr = sinf(cam_roll);

    draw_backdrop();
    float R[9];
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *k = &rocks[i];
        if (!k->alive) continue;
        int lod = k->z < 220 ? 2 : k->z < 550 ? 1 : 0;
        if (lod > q->lod) lod = q->lod;
        rot_matrix(R, k->ay, k->ax, k->az);
        draw_mesh(&rock_mesh[k->var][lod], R, k->r, k->x, k->y, k->z, k->r * 1.4f, q->smooth);
    }
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;
        rot_matrix(R, 0, e->phase == 2 ? -0.5f : 0.0f, e->bank);
        if (e->type) draw_mesh(&cruiser_mesh, R, 1.4f, e->x, e->y, e->z, 20, false);
        else draw_mesh(&fighter_mesh, R, 1.3f, e->x, e->y, e->z, 10, false);
    }
    for (int i = 0; i < MAX_PICKUPS; i++) {
        Pickup *p = &pickups[i];
        if (!p->alive) continue;
        rot_matrix(R, p->a, 0.3f, 0);
        draw_mesh(&pickup_mesh, R, 1.0f, p->x, p->y, p->z, 4, false);
    }
    for (int i = 0; i < ndebris; i++) draw_debris(&debris[i]);
    if (ship_alive && (invuln <= 0 || ((int)(invuln * 20) & 1) == 0)) {
        float bank = -pvx * 0.012f;
        if (roll_t > 0) bank += (1 - roll_t / 0.55f) * 2 * PI;
        rot_matrix(R, pvx * 0.004f, -pvy * 0.006f, bank);
        draw_mesh(&ship_mesh, R, 1.0f, px, py, PZ, 8, false);
    }
    for (int b = NBUCKET - 1; b >= 0; b--)
        for (int t = bucket[b]; t >= 0; t = tris[t].next) raster(&tris[t]);

    float sx, sy, ex, ey;
    for (int i = 0; i < MAX_LASERS; i++) {
        Shot *s = &lasers[i];
        if (!s->alive) continue;
        if (project(s->x, s->y, s->z, &sx, &sy) && project(s->x - s->vx * 0.03f, s->y, s->z - 15, &ex, &ey))
            line2(sx, sy, ex, ey, RGB(40, 255, 80), RGB(220, 255, 220));
    }
    for (int i = 0; i < MAX_BOLTS; i++) {
        Shot *b = &bolts[i];
        if (!b->alive || !project(b->x, b->y, b->z, &sx, &sy)) continue;
        int r = (int)(2.2f * FOCAL / b->z);
        if (r < 1) r = 1;
        disc((int)sx, (int)sy, r + 1, RGB(255, 40, 120), false);
        disc((int)sx, (int)sy, r / 2, RGB(255, 220, 240), false);
    }
    for (int i = 0; i < nsparks; i++) {
        Spark *s = &sparks[i];
        if (!project(s->x, s->y, s->z, &sx, &sy)) continue;
        color_t c = s->life > 0.5f ? RGB(255, 255, 160) : s->life > 0.25f ? RGB(255, 180, 40) : RGB(220, 70, 20);
        int x = (int)sx, y = (int)sy;
        plot(x, y, c);
        if (s->z < 200) { plot(x + 1, y, c); plot(x, y + 1, c); plot(x + 1, y + 1, c); }
    }
    for (int i = 0; i < MAX_FLASH; i++) {
        Flash *f = &flashes[i];
        if (f->life <= 0 || !project(f->x, f->y, f->z, &sx, &sy)) continue;
        int r = (int)(f->r * FOCAL / f->z * (1.3f - f->life * 2));
        if (r > 0) disc((int)sx, (int)sy, r, f->life > 0.2f ? RGB(255, 255, 200) : RGB(255, 160, 40), true);
    }
    if (ship_alive && state != ST_OVER && project(px, py, PZ + 200, &sx, &sy)) {
        int x = (int)sx, y = (int)sy;
        color_t c = RGB(80, 255, 120);
        for (int k = 4; k < 9; k++) { plot(x - k, y - 8, c); plot(x + k, y - 8, c); plot(x - k, y + 8, c); plot(x + k, y + 8, c);
                                      plot(x - 8, y - k, c); plot(x - 8, y + k, c); plot(x + 8, y - k, c); plot(x + 8, y + k, c); }
        plot(x, y, c);
    }
    if (hit_flash > 0)
        for (int k = 0; k < 4; k++) {
            span_flat(fb_ptr + k * 320, 320, RED);
            span_flat(fb_ptr + (VIEW_H - 1 - k) * 320, 320, RED);
            for (int y = 0; y < VIEW_H; y++) { fb_ptr[y * 320 + k] = RED; fb_ptr[y * 320 + 319 - k] = RED; }
        }
    cam_x = save_x;
    cam_y = save_y;
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
        center_text(28, "N O V A   S T R I K E", WHITE);
        gfx_font(FONT_6X10);
        center_text(48, "a polygon space shooter", LIGHTGRAY);
        if (best > 0) { snprintf(buf, sizeof buf, "best %d", best); center_text(64, buf, CYAN); }
        if ((sys_millis() / 400) & 1) center_text(190, "press ENTER", YELLOW);
        center_text(208, "arrows fly  SPACE/x fire  z roll  i invert", WHITE);
        center_text(222, "q quality  m mute  ESC quit", WHITE);
        return;
    }
    gfx_font(FONT_7X13);
    snprintf(buf, sizeof buf, "%d", score);
    gfx_text(9, 7, buf, BLACK);
    gfx_text(8, 6, buf, WHITE);
    gfx_font(FONT_6X10);
    int w = (int)(shields * 0.8f);
    gfx_rect(230, 6, 84, 9, WHITE);
    if (w > 0) gfx_fill_rect(232, 8, w, 5, shields > 50 ? GREEN : shields > 25 ? YELLOW : RED);
    if (banner_t > 0 && wave > 0) {
        gfx_font(FONT_7X13);
        snprintf(buf, sizeof buf, "W A V E  %d", wave);
        center_text(70, buf, YELLOW);
        gfx_font(FONT_6X10);
    }
    if (state == ST_OVER) {
        gfx_fill_rect(80, 70, 160, 90, RGB(10, 14, 30));
        gfx_rect(80, 70, 160, 90, WHITE);
        gfx_font(FONT_7X13);
        center_text(78, "GAME OVER", RED);
        gfx_font(FONT_6X10);
        snprintf(buf, sizeof buf, "score %d  wave %d", score, wave);
        center_text(100, buf, WHITE);
        snprintf(buf, sizeof buf, "%d kills", kills);
        center_text(114, buf, LIGHTGRAY);
        if (new_best) center_text(128, "new record!", YELLOW);
        if (dead_t > 3.5f) center_text(144, "ENTER to continue", LIGHTGRAY);
    }
}

static void draw_hud(void)
{
    gfx_fill_rect(0, HUD_Y, 320, 320 - HUD_Y, RGB(12, 14, 26));
    gfx_line(0, HUD_Y, 319, HUD_Y, RGB(70, 80, 110));
    /* radar: seen from above, the ship at the bottom centre */
    int rx = 2, ry = HUD_Y + 2;
    gfx_fill_rect(rx, ry, 76, 76, RGB(6, 24, 14));
    gfx_rect(rx, ry, 76, 76, RGB(30, 90, 50));
    gfx_line(rx + 38, ry, rx + 38, ry + 75, RGB(20, 60, 34));
    for (int i = 0; i < MAX_ROCKS; i++)
        if (rocks[i].alive && rocks[i].z > 0) {
            int x = rx + 38 + (int)(rocks[i].x * 38 / 260), y = ry + 72 - (int)(rocks[i].z * 70 / vfar);
            if (x > rx && x < rx + 75 && y > ry) gfx_pixel(x, y, GRAY);
        }
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].alive && enemies[i].z > 0) {
            int x = rx + 38 + (int)(enemies[i].x * 38 / 260), y = ry + 72 - (int)(enemies[i].z * 70 / vfar);
            if (x > rx && x < rx + 74 && y > ry) gfx_fill_rect(x - 1, y - 1, 3, 3, enemies[i].type ? ORANGE : RED);
        }
    for (int i = 0; i < MAX_PICKUPS; i++)
        if (pickups[i].alive && pickups[i].z > 0) {
            int x = rx + 38 + (int)(pickups[i].x * 38 / 260), y = ry + 72 - (int)(pickups[i].z * 70 / vfar);
            if (x > rx && x < rx + 74 && y > ry) gfx_fill_rect(x - 1, y - 1, 3, 3, GREEN);
        }
    gfx_fill_rect(rx + 37 + (int)(px * 38 / 260), ry + 70, 3, 3, WHITE);

    int tx = 86, ty = HUD_Y + 5;
    gfx_printf(tx, ty, WHITE, "SCORE %d", score);
    gfx_printf(tx, ty + 12, LIGHTGRAY, "BEST  %d", best);
    gfx_printf(tx, ty + 24, LIGHTGRAY, "WAVE %d  K %d", wave, kills);
    gfx_text(tx, ty + 38, "SHIELD", LIGHTGRAY);
    gfx_fill_rect(tx, ty + 50, 100, 8, RGB(40, 44, 56));
    int w = (int)(shields * 0.96f);
    if (w > 0) gfx_fill_rect(tx + 2, ty + 52, w, 4, shields > 50 ? GREEN : shields > 25 ? YELLOW : RED);
    if (state == ST_TITLE) gfx_text(tx, ty + 62, "attract mode", GRAY);

    int x = 196;
    const Quality *q = &qual[quality];
    gfx_printf(x, ty, fps >= 25 ? GREEN : fps >= 15 ? YELLOW : RED, "FPS %5.1f", (double)fps);
    gfx_printf(x, ty + 12, LIGHTGRAY, "3D  %5.1f ms", (double)ms_render);
    gfx_printf(x, ty + 24, LIGHTGRAY, "LCD %5.1f ms", (double)ms_lcd);
    gfx_printf(x, ty + 36, CYAN, "Q %s lod%d %s", q->name, q->lod, q->smooth ? "smooth" : "flat");
    gfx_printf(x, ty + 48, GRAY, "tris %d v %d", ntris, nverts);
    gfx_printf(x, ty + 60, GRAY, "rocks %d heap %uK", q->rocks, (unsigned)(sys_free_heap() / 1024));
}

/* ============================================================ main */

void setup(void)
{
    gfx_clear(BLACK);
    gfx_font(FONT_7X13);
    center_text(150, "building models...", WHITE);
    gfx_font(FONT_6X10);
    gfx_flush();
    tris = malloc(MAX_TRIS * sizeof(Tri));
    if (!tris) {
        gfx_clear(BLACK);
        gfx_text(10, 10, "not enough memory", RED);
        gfx_flush();
        sys_delay(2000);
        sys_exit();
    }
    FILE *f = fopen(HISCORE_FILE, "r");
    if (f) {
        if (fscanf(f, "%d", &best) != 1) best = 0;
        fclose(f);
    }
    sys_seed(sys_millis() ^ 0x5F1);
    build_meshes();
    build_palettes();
    vfar = qual[quality].far;
    for (int i = 0; i < NSTARS; i++) { stars[i].x = frand(-900, 900); stars[i].y = frand(-700, 700); stars[i].z = frand(20, 1520); }
    for (int i = 0; i < qual[quality].rocks; i++) spawn_rock(&rocks[i], frand(80, vfar));
    reset_world();
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
        if (k == 'i') invert = !invert;
        if (k == 'z' && state == ST_PLAY && roll_t <= 0) roll_t = 0.55f;
        if (k == KEY_ENTER && state == ST_TITLE) { reset_world(); state = ST_PLAY; }
        else if (k == KEY_ENTER && state == ST_OVER && dead_t > 3.5f) { reset_world(); state = ST_TITLE; }
    }

    uint32_t now = sys_millis();
    int steps = 0;
    while (now - last_ms >= STEP_MS && steps < 4) { step(); last_ms += STEP_MS; steps++; }
    if (now - last_ms > STEP_MS * 4) last_ms = now;
    if (state == ST_OVER) dead_t += (float)steps * DT;
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
