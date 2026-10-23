/* solids - real-time flat-shaded 3D: rotating meshes built at run time (cube, octahedron,
 * icosahedron, geodesic sphere by subdivision, torus, twisted star), rotation matrices, perspective
 * projection, back-face culling, painter's algorithm (qsort on depth), Lambert + Blinn specular
 * lighting into RGB565, optional wireframe overlay and a perspective floor grid.
 *   1-6 shape   SPACE pause   w wireframe (edges / lines only / off)   arrows spin   +/- zoom   ESC */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } Vec3;
typedef struct { int a, b, c; color_t color; } Face;
typedef struct {
    const char *name;
    Vec3 *v;
    Face *f;
    int nv, nf, cap_v, cap_f;
} Mesh;

typedef struct { float m[3][3]; } Mat3;

#define NMESH 6
static Mesh meshes[NMESH];
static int cur;
static float ax = 0.4f, ay = 0.3f, az, spin_x = 0.7f, spin_y = 1.1f, dist = 4.2f;
static bool paused, wire, wire_only;
static uint32_t last_ms, frame_us;
static int drawn;

/* per-frame scratch, sized for the largest mesh */
static Vec3 *tv;               /* view-space vertices */
static float *sx, *sy;         /* projected */
static int *order;
static float *depth;

/* ---------------------------------------------------------------- vector math */

static Vec3 v3(float x, float y, float z) { Vec3 r = { x, y, z }; return r; }
static Vec3 sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec3 add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec3 scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vec3 normalize(Vec3 a)
{
    float l = sqrtf(dot(a, a));
    return l > 0 ? scale(a, 1.0f / l) : a;
}

static Mat3 mat_mul(Mat3 a, Mat3 b)
{
    Mat3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

static Mat3 rot_x(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat3 r = { { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } } };
    return r;
}
static Mat3 rot_y(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat3 r = { { { c, 0, s }, { 0, 1, 0 }, { -s, 0, c } } };
    return r;
}
static Mat3 rot_z(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat3 r = { { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } } };
    return r;
}

static Vec3 mat_apply(const Mat3 *m, Vec3 p)
{
    return v3(m->m[0][0] * p.x + m->m[0][1] * p.y + m->m[0][2] * p.z,
              m->m[1][0] * p.x + m->m[1][1] * p.y + m->m[1][2] * p.z,
              m->m[2][0] * p.x + m->m[2][1] * p.y + m->m[2][2] * p.z);
}

/* ------------------------------------------------------------------- meshes */

static int add_vert(Mesh *m, Vec3 p)
{
    if (m->nv == m->cap_v) {
        m->cap_v = m->cap_v ? m->cap_v * 2 : 16;
        m->v = realloc(m->v, (size_t)m->cap_v * sizeof(Vec3));
    }
    m->v[m->nv] = p;
    return m->nv++;
}

static void add_face(Mesh *m, int a, int b, int c, color_t col)
{
    if (m->nf == m->cap_f) {
        m->cap_f = m->cap_f ? m->cap_f * 2 : 16;
        m->f = realloc(m->f, (size_t)m->cap_f * sizeof(Face));
    }
    Face f = { a, b, c, col };
    m->f[m->nf++] = f;
}

static void add_quad(Mesh *m, int a, int b, int c, int d, color_t col)
{
    add_face(m, a, b, c, col);
    add_face(m, a, c, d, col);
}

/* convex solids: make every face wind counter-clockwise seen from outside */
static void orient_outward(Mesh *m)
{
    for (int i = 0; i < m->nf; i++) {
        Face *f = &m->f[i];
        Vec3 a = m->v[f->a], b = m->v[f->b], c = m->v[f->c];
        Vec3 n = cross(sub(b, a), sub(c, a));
        if (dot(n, add(add(a, b), c)) < 0) { int t = f->b; f->b = f->c; f->c = t; }
    }
}

/* faces of a regular solid = vertex triples whose three edges all have the solid's edge length */
static void faces_from_edges(Mesh *m, float edge, const color_t *palette, int npal)
{
    float e2 = edge * edge;
    for (int i = 0; i < m->nv; i++)
        for (int j = i + 1; j < m->nv; j++) {
            Vec3 d = sub(m->v[i], m->v[j]);
            if (fabsf(dot(d, d) - e2) > 0.01f) continue;
            for (int k = j + 1; k < m->nv; k++) {
                Vec3 d1 = sub(m->v[i], m->v[k]), d2 = sub(m->v[j], m->v[k]);
                if (fabsf(dot(d1, d1) - e2) < 0.01f && fabsf(dot(d2, d2) - e2) < 0.01f)
                    add_face(m, i, j, k, palette[m->nf % npal]);
            }
        }
    orient_outward(m);
}

static void build_cube(Mesh *m)
{
    m->name = "cube";
    for (int i = 0; i < 8; i++) add_vert(m, v3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1));
    static const int q[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    static const color_t col[6] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA };
    for (int i = 0; i < 6; i++) add_quad(m, q[i][0], q[i][1], q[i][2], q[i][3], col[i]);
    orient_outward(m);
}

static void build_octahedron(Mesh *m)
{
    static const color_t pal[2] = { RGB(255, 140, 40), RGB(255, 220, 120) };
    m->name = "octahedron";
    const float s = 1.4f;
    add_vert(m, v3(s, 0, 0)); add_vert(m, v3(-s, 0, 0));
    add_vert(m, v3(0, s, 0)); add_vert(m, v3(0, -s, 0));
    add_vert(m, v3(0, 0, s)); add_vert(m, v3(0, 0, -s));
    faces_from_edges(m, s * 1.41421356f, pal, 2);
}

static void icosa_verts(Mesh *m, float r)
{
    const float p = 1.61803399f, k = r / sqrtf(1 + p * p);
    for (int i = 0; i < 4; i++) {
        float a = (i & 1 ? -1 : 1) * k, b = (i & 2 ? -p : p) * k;
        add_vert(m, v3(0, a, b));
        add_vert(m, v3(a, b, 0));
        add_vert(m, v3(b, 0, a));
    }
}

static void build_icosahedron(Mesh *m)
{
    static const color_t pal[3] = { RGB(80, 200, 255), RGB(40, 120, 230), RGB(150, 230, 255) };
    m->name = "icosahedron";
    icosa_verts(m, 1.5f);
    faces_from_edges(m, 2 * 1.5f / sqrtf(1 + 1.61803399f * 1.61803399f), pal, 3);
}

/* geodesic sphere: split each icosahedron face into 4, push the new vertices onto the sphere, twice */
static void build_geosphere(Mesh *m)
{
    m->name = "geodesic sphere";
    Mesh base = { 0 };
    build_icosahedron(&base);
    for (int i = 0; i < base.nv; i++) add_vert(m, base.v[i]);
    for (int i = 0; i < base.nf; i++) add_face(m, base.f[i].a, base.f[i].b, base.f[i].c, 0);
    free(base.v);
    free(base.f);
    for (int pass = 0; pass < 2; pass++) {
        int nf = m->nf;
        Face *old = malloc((size_t)nf * sizeof(Face));
        memcpy(old, m->f, (size_t)nf * sizeof(Face));
        m->nf = 0;
        for (int i = 0; i < nf; i++) {
            Vec3 a = m->v[old[i].a], b = m->v[old[i].b], c = m->v[old[i].c];
            int ab = add_vert(m, scale(normalize(add(a, b)), 1.5f));
            int bc = add_vert(m, scale(normalize(add(b, c)), 1.5f));
            int ca = add_vert(m, scale(normalize(add(c, a)), 1.5f));
            add_face(m, old[i].a, ab, ca, 0);
            add_face(m, old[i].b, bc, ab, 0);
            add_face(m, old[i].c, ca, bc, 0);
            add_face(m, ab, bc, ca, 0);
        }
        free(old);
    }
    /* colour bands by latitude */
    for (int i = 0; i < m->nf; i++) {
        float y = (m->v[m->f[i].a].y + m->v[m->f[i].b].y + m->v[m->f[i].c].y) / 4.5f;
        int band = (int)((y + 1.0f) * 4.0f);
        m->f[i].color = band & 1 ? RGB(60, 220, 120) : RGB(30, 150, 200);
    }
}

static void build_torus(Mesh *m)
{
    m->name = "torus";
    const int seg = 24, ring = 12;
    const float R = 1.25f, r = 0.5f;
    for (int i = 0; i < seg; i++) {
        float u = (float)i / seg * 6.2831853f;
        for (int j = 0; j < ring; j++) {
            float v = (float)j / ring * 6.2831853f;
            add_vert(m, v3((R + r * cosf(v)) * cosf(u), r * sinf(v), (R + r * cosf(v)) * sinf(u)));
        }
    }
    for (int i = 0; i < seg; i++)
        for (int j = 0; j < ring; j++) {
            int a = i * ring + j, b = ((i + 1) % seg) * ring + j;
            int c = ((i + 1) % seg) * ring + (j + 1) % ring, d = i * ring + (j + 1) % ring;
            add_quad(m, a, d, c, b, ((i / 2 + j / 3) & 1) ? RGB(230, 70, 160) : RGB(250, 200, 60));
        }
}

/* a star: an icosahedron with a spike raised over every face, twisted a little */
static void build_star(Mesh *m)
{
    m->name = "stellated star";
    Mesh base = { 0 };
    build_icosahedron(&base);
    for (int i = 0; i < base.nv; i++) add_vert(m, scale(base.v[i], 0.8f));
    for (int i = 0; i < base.nf; i++) {
        Face f = base.f[i];
        Vec3 c = scale(add(add(m->v[f.a], m->v[f.b]), m->v[f.c]), 1.0f / 3.0f);
        Vec3 tip = scale(normalize(c), 1.9f);
        tip = add(tip, scale(normalize(sub(m->v[f.a], c)), 0.15f));
        int t = add_vert(m, tip);
        color_t col = (i & 1) ? RGB(255, 230, 90) : RGB(255, 120, 40);
        add_face(m, f.a, f.b, t, col);
        add_face(m, f.b, f.c, t, col);
        add_face(m, f.c, f.a, t, col);
    }
    free(base.v);
    free(base.f);
}

/* ------------------------------------------------------------------ drawing */

static color_t light_color(color_t base, float diffuse, float spec)
{
    int r = (base >> 11) << 3, g = ((base >> 5) & 63) << 2, b = (base & 31) << 3;
    float k = 0.16f + 0.84f * diffuse;
    int s = (int)(spec * 255.0f);
    r = (int)((float)r * k) + s; g = (int)((float)g * k) + s; b = (int)((float)b * k) + s;
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

static int far_first(const void *a, const void *b)
{
    float da = depth[*(const int *)a], db = depth[*(const int *)b];
    return da < db ? 1 : da > db ? -1 : 0;
}

static void draw_background(void)
{
    for (int y = 0; y < SCREEN_H; y++) {
        int t = y * 64 / SCREEN_H;
        gfx_hline(0, y, SCREEN_W, RGB(8 + t / 2, 10 + t / 2, 30 + t));
    }
    /* floor grid in perspective (camera looks slightly down at y = -2.2) */
    const float fy = -2.2f, f = 260.0f;
    for (int i = -6; i <= 6; i++) {
        float x = (float)i;
        int x0 = 160 + (int)(x * f / 2.0f), y0 = 160 - (int)(fy * f / 2.0f);
        int x1 = 160 + (int)(x * f / 14.0f), y1 = 160 - (int)(fy * f / 14.0f);
        gfx_line(x0, y0, x1, y1, RGB(40, 50, 110));
    }
    for (float z = 2.0f; z <= 14.0f; z += 1.5f) {
        int y = 160 - (int)(fy * f / z);
        gfx_hline(0, y, SCREEN_W, RGB(40, 50, 110));
    }
}

static void render(void)
{
    Mesh *m = &meshes[cur];
    Mat3 rot = mat_mul(mat_mul(rot_z(az), rot_y(ay)), rot_x(ax));
    const float f = 260.0f;
    for (int i = 0; i < m->nv; i++) {
        Vec3 p = mat_apply(&rot, m->v[i]);
        p.z += dist;
        tv[i] = p;
        sx[i] = 160.0f + p.x * f / p.z;
        sy[i] = 150.0f - p.y * f / p.z;
    }
    /* cull back faces in screen space, sort the rest far to near */
    int n = 0;
    for (int i = 0; i < m->nf; i++) {
        Face *fc = &m->f[i];
        float cr = (sx[fc->b] - sx[fc->a]) * (sy[fc->c] - sy[fc->a]) - (sy[fc->b] - sy[fc->a]) * (sx[fc->c] - sx[fc->a]);
        if (cr <= 0 && !wire_only) continue;   /* screen y points down: front faces come out positive */
        depth[i] = tv[fc->a].z + tv[fc->b].z + tv[fc->c].z;
        order[n++] = i;
    }
    qsort(order, (size_t)n, sizeof order[0], far_first);
    drawn = n;

    Vec3 L = normalize(v3(-0.5f, 0.7f, -0.6f));            /* towards the light, view space */
    Vec3 H = normalize(add(L, v3(0, 0, -1)));              /* Blinn half vector, viewer at -z */
    for (int k = 0; k < n; k++) {
        Face *fc = &m->f[order[k]];
        int x0 = (int)sx[fc->a], y0 = (int)sy[fc->a], x1 = (int)sx[fc->b], y1 = (int)sy[fc->b];
        int x2 = (int)sx[fc->c], y2 = (int)sy[fc->c];
        if (!wire_only) {
            Vec3 nrm = normalize(cross(sub(tv[fc->b], tv[fc->a]), sub(tv[fc->c], tv[fc->a])));   /* outward */
            float d = dot(nrm, L), s = dot(nrm, H);
            if (d < 0) d = 0;
            if (s < 0) s = 0;
            s *= s; s *= s; s *= s; s *= s; s *= s; s *= s;    /* s^64 */
            gfx_fill_triangle(x0, y0, x1, y1, x2, y2, light_color(fc->color, d, s * 0.5f));
        }
        if (wire || wire_only) {
            color_t c = wire_only ? GREEN : RGB(10, 10, 20);
            gfx_line(x0, y0, x1, y1, c);
            gfx_line(x1, y1, x2, y2, c);
            gfx_line(x2, y2, x0, y0, c);
        }
    }
}

static void draw(void)
{
    uint64_t t0 = sys_micros();
    draw_background();
    render();
    Mesh *m = &meshes[cur];
    gfx_font(FONT_7X13);
    gfx_text(8, 6, m->name, WHITE);
    gfx_font(FONT_6X10);
    gfx_printf(8, 22, LIGHTGRAY, "%d verts  %d faces  %d drawn", m->nv, m->nf, drawn);
    gfx_printf(8, 34, LIGHTGRAY, "frame %u.%u ms", (unsigned)(frame_us / 1000), (unsigned)(frame_us / 100 % 10));
    gfx_text(8, 296, "1-6 shape  w wire  SPACE pause", GRAY);
    gfx_text(8, 306, "arrows spin  +/- zoom  ESC quit", GRAY);
    gfx_flush();
    frame_us = (uint32_t)(sys_micros() - t0);
}

void setup(void)
{
    build_cube(&meshes[0]);
    build_octahedron(&meshes[1]);
    build_icosahedron(&meshes[2]);
    build_geosphere(&meshes[3]);
    build_torus(&meshes[4]);
    build_star(&meshes[5]);
    int maxv = 0, maxf = 0;
    for (int i = 0; i < NMESH; i++) {
        if (meshes[i].nv > maxv) maxv = meshes[i].nv;
        if (meshes[i].nf > maxf) maxf = meshes[i].nf;
    }
    tv = malloc((size_t)maxv * sizeof(Vec3));
    sx = malloc((size_t)maxv * sizeof(float));
    sy = malloc((size_t)maxv * sizeof(float));
    order = malloc((size_t)maxf * sizeof(int));
    depth = malloc((size_t)maxf * sizeof(float));
    last_ms = sys_millis();
}

static void quit(void)
{
    for (int i = 0; i < NMESH; i++) { free(meshes[i].v); free(meshes[i].f); }
    free(tv); free(sx); free(sy); free(order); free(depth);
    sys_exit();
}

void loop(void)
{
    int k = kbd_get();
    switch (k) {
    case KEY_ESC: quit(); break;
    case ' ': paused = !paused; break;
    case 'w':
        if (wire_only) { wire_only = false; wire = false; }
        else if (wire) wire_only = true;
        else wire = true;
        break;
    case '+': case '=': if (dist > 2.6f) dist -= 0.3f; break;
    case '-': if (dist < 9.0f) dist += 0.3f; break;
    default:
        if (k >= '1' && k < '1' + NMESH) cur = k - '1';
    }
    uint32_t now = sys_millis();
    float dt = (float)(now - last_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;
    if (kbd_down(KEY_LEFT)) ay -= 2.0f * dt;
    if (kbd_down(KEY_RIGHT)) ay += 2.0f * dt;
    if (kbd_down(KEY_UP)) ax -= 2.0f * dt;
    if (kbd_down(KEY_DOWN)) ax += 2.0f * dt;
    if (!paused) {
        ax += spin_x * dt * 0.6f;
        ay += spin_y * dt * 0.6f;
        az += 0.25f * dt;
    }
    draw();
    sys_yield();
}
