/* raycaster - textured first-person maze in the style of Wolfenstein 3D.
 * DDA ray casting per screen column, procedurally generated wall/floor textures, per-row floor and
 * ceiling casting, distance fog, billboard sprites clipped against a z-buffer (sorted with qsort),
 * collision with wall sliding, frame-rate independent movement through kbd_down().
 * Collect every orb to clear the maze.
 *   UP/DOWN walk   LEFT/RIGHT turn   z/x strafe   f floor on/off   ENTER restart   ESC quit */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_W 320
#define VIEW_H 200
#define TEX 32
#define MAP_W 24
#define MAP_H 24
#define PLANE 0.66f                          /* camera plane half-width: ~66 degree FOV */
#define PROJ ((VIEW_W / 2) / PLANE)          /* pixels per world unit at distance 1 */
#define MAX_SPRITES 32
#define PILLAR 9

enum { T_BRICK, T_STONE, T_WOOD, T_METAL, T_FLOOR, T_CEIL, T_PILLAR, T_ORB, NTEX };

/* 1 brick  2 stone  3 wood  4 metal  c pillar  o orb  P start */
static const char *const level[MAP_H] = {
    "111111111111111111111111",
    "1P.....1.......4444444.4",
    "1......1..o....4.....4.4",
    "1..c...1.......4..o..4.4",
    "1......1111.1114.....4.4",
    "1...............4.4444.4",
    "1......c...c...........4",
    "1222.2222......4444444.4",
    "1......2...........3...4",
    "1..o...2..c...c....3.o.4",
    "1......2...........3...4",
    "1......2222.2222...333.4",
    "1..............2.......4",
    "13333.333......2...c...4",
    "1...3...3..o...2.......4",
    "1...3...3......22222.224",
    "1...3.o.3..............4",
    "1...3...3..c...c...c...4",
    "1...33333..............4",
    "1..........1111.11111114",
    "1..c.......1...........4",
    "1..........1...o.....c.4",
    "1..........1...........4",
    "111111111111111111111111",
};

typedef struct {
    float x, y;
    int tex;
    float scale, z;          /* size in world units, height of its centre (0.5 = eye level) */
    bool alive;
    float dist;
} Sprite;

static uint8_t map[MAP_H][MAP_W];
static color_t *tex[NTEX];
static Sprite sprites[MAX_SPRITES];
static Sprite *order[MAX_SPRITES];
static int nsprites, orbs_total, orbs_left;
static float posx, posy, dirx, diry, planex, planey;
static float zbuf[VIEW_W];
static float row_dist[VIEW_H];
static bool floor_on = true;
static uint32_t last_ms, start_ms, finish_ms, fps_ms;
static int frames, fps;

/* ------------------------------------------------------------------ textures */

static uint32_t hash2(int x, int y, int seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int noise(int x, int y, int seed, int amp) { return (int)(hash2(x, y, seed) % (uint32_t)(2 * amp + 1)) - amp; }

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static color_t rgb(int r, int g, int b) { return RGB(clamp255(r), clamp255(g), clamp255(b)); }

/* RGB565 brightness scale, s = 0..32 (32 = unchanged): red/blue and green never overlap */
static color_t shade(color_t c, int s)
{
    uint32_t rb = c & 0xF81Fu, g = c & 0x07E0u;
    return (color_t)((((rb * (uint32_t)s) >> 5) & 0xF81Fu) | (((g * (uint32_t)s) >> 5) & 0x07E0u));
}

static void make_textures(void)
{
    for (int t = 0; t < NTEX; t++) tex[t] = malloc(TEX * TEX * sizeof(color_t));
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            int i = y * TEX + x, n = noise(x, y, 1, 18);
            /* brick: rows of 8, every other row offset by half a brick */
            int bx = (x + ((y / 8) & 1) * 8) % 16;
            bool mortar = y % 8 == 7 || bx == 15;
            tex[T_BRICK][i] = mortar ? rgb(150 + n, 150 + n, 140 + n)
                                     : rgb(160 + n + noise(x / 16, y / 8, 2, 25), 50 + n / 2, 40 + n / 2);
            /* stone: irregular cells from blocky noise */
            int cell = noise(x / 6, y / 5, 3, 30);
            bool crack = x % 6 == 0 || y % 5 == 0;
            int v = crack ? 70 : 125 + cell + n;
            tex[T_STONE][i] = rgb(v, v, v + 8);
            /* wood: vertical planks with sine grain */
            int grain = (int)(18.0f * sinf((float)y * 0.35f + (float)(x % 8) * 1.7f + (float)(x / 8)));
            tex[T_WOOD][i] = x % 8 == 0 ? rgb(60, 35, 15) : rgb(140 + grain + n / 2, 90 + grain / 2 + n / 3, 40);
            /* metal: panels with a bevel and rivets */
            int px = x % 16, py = y % 16;
            int m = 90 + n / 3 + (px == 0 || py == 0 ? 60 : px == 15 || py == 15 ? -40 : 0);
            bool rivet = (px == 3 || px == 12) && (py == 3 || py == 12);
            tex[T_METAL][i] = rivet ? rgb(220, 220, 230) : rgb(m - 20, m, m + 40);
            /* floor: two-tone tiles */
            bool dark = ((x / 16) ^ (y / 16)) & 1;
            int f = (dark ? 70 : 110) + n / 2 + (x % 16 == 0 || y % 16 == 0 ? -40 : 0);
            tex[T_FLOOR][i] = rgb(f, f - 10, f - 25);
            /* ceiling: dark boards */
            tex[T_CEIL][i] = rgb(45 + n / 3 + (y % 8 == 0 ? -20 : 0), 40 + n / 3, 50 + n / 3);
            /* pillar sprite: a shaded column, transparent (0) outside it */
            float dx = ((float)x - 15.5f) / 8.0f;
            if (fabsf(dx) <= 1.0f) {
                int l = (int)(170.0f * sqrtf(1.0f - dx * dx)) + 40 + n / 2 + (y < 3 || y > 28 ? 30 : 0);
                tex[T_PILLAR][i] = rgb(l, l, l - 10) | 0x0821;   /* never pure black */
            } else
                tex[T_PILLAR][i] = 0;
            /* orb sprite: glowing ball */
            float ox = ((float)x - 15.5f) / 15.5f, oy = ((float)y - 15.5f) / 15.5f;
            float d2 = ox * ox + oy * oy;
            if (d2 < 1.0f) {
                float k = 1.0f - d2, hl = (ox + 0.35f) * (ox + 0.35f) + (oy + 0.35f) * (oy + 0.35f);
                int h = hl < 0.06f ? 120 : 0;
                tex[T_ORB][i] = rgb((int)(255.0f * k) + h, (int)(220.0f * k) + h, (int)(60.0f * k * k) + h) | 0x0821;
            } else
                tex[T_ORB][i] = 0;
        }
}

/* --------------------------------------------------------------------- world */

static void add_sprite(float x, float y, int t, float scale, float z)
{
    if (nsprites == MAX_SPRITES) return;
    Sprite *s = &sprites[nsprites++];
    s->x = x; s->y = y; s->tex = t; s->scale = scale; s->z = z; s->alive = true;
}

static void reset_world(void)
{
    nsprites = orbs_total = 0;
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            char c = level[y][x];
            map[y][x] = 0;
            if (c >= '1' && c <= '4') map[y][x] = (uint8_t)(c - '0');
            else if (c == 'c') { map[y][x] = PILLAR; add_sprite(x + 0.5f, y + 0.5f, T_PILLAR, 1.0f, 0.5f); }
            else if (c == 'o') { add_sprite(x + 0.5f, y + 0.5f, T_ORB, 0.35f, 0.35f); orbs_total++; }
            else if (c == 'P') { posx = x + 0.5f; posy = y + 0.5f; }
        }
    orbs_left = orbs_total;
    dirx = 1; diry = 0;
    planex = 0; planey = PLANE;
    start_ms = sys_millis();
    finish_ms = 0;
}

static bool solid(float x, float y) { return map[(int)y][(int)x] != 0; }

static void move(float dx, float dy)
{
    const float r = 0.22f;   /* body radius: test each axis separately so walls make you slide */
    if (!solid(posx + dx + (dx > 0 ? r : -r), posy)) posx += dx;
    if (!solid(posx, posy + dy + (dy > 0 ? r : -r))) posy += dy;
}

static void rotate(float a)
{
    float c = cosf(a), s = sinf(a), t;
    t = dirx; dirx = t * c - diry * s; diry = t * s + diry * c;
    t = planex; planex = t * c - planey * s; planey = t * s + planey * c;
}

static void update(float dt)
{
    float speed = 2.6f * dt, turn = 2.2f * dt;
    if (kbd_down(KEY_UP)) move(dirx * speed, diry * speed);
    if (kbd_down(KEY_DOWN)) move(-dirx * speed, -diry * speed);
    if (kbd_down(KEY_LEFT)) rotate(-turn);
    if (kbd_down(KEY_RIGHT)) rotate(turn);
    if (kbd_down('z')) move(diry * speed, -dirx * speed);
    if (kbd_down('x')) move(-diry * speed, dirx * speed);
    for (int i = 0; i < nsprites; i++) {
        Sprite *s = &sprites[i];
        if (!s->alive || s->tex != T_ORB) continue;
        float dx = s->x - posx, dy = s->y - posy;
        if (dx * dx + dy * dy < 0.16f) {
            s->alive = false;
            orbs_left--;
            snd_tone(orbs_left ? 880 : 1320, orbs_left ? 60 : 400);
            if (!orbs_left) finish_ms = sys_millis();
        }
    }
}

/* ------------------------------------------------------------------- drawing */

static void draw_walls(color_t *fb)
{
    for (int x = 0; x < VIEW_W; x++) {
        float cam = 2.0f * (float)x / VIEW_W - 1.0f;
        float rdx = dirx + planex * cam, rdy = diry + planey * cam;
        int mx = (int)posx, my = (int)posy;
        float ddx = rdx == 0 ? 1e30f : fabsf(1.0f / rdx), ddy = rdy == 0 ? 1e30f : fabsf(1.0f / rdy);
        int sx = rdx < 0 ? -1 : 1, sy = rdy < 0 ? -1 : 1;
        float sdx = rdx < 0 ? (posx - mx) * ddx : (mx + 1.0f - posx) * ddx;
        float sdy = rdy < 0 ? (posy - my) * ddy : (my + 1.0f - posy) * ddy;
        int side, cell;
        for (;;) {   /* the map border is solid, so this ends */
            if (sdx < sdy) { sdx += ddx; mx += sx; side = 0; }
            else { sdy += ddy; my += sy; side = 1; }
            cell = map[my][mx];
            if (cell && cell != PILLAR) break;
        }
        float perp = side == 0 ? sdx - ddx : sdy - ddy;
        if (perp < 0.05f) perp = 0.05f;
        zbuf[x] = perp;

        int h = (int)(PROJ / perp);
        int y0 = VIEW_H / 2 - h / 2, y1 = y0 + h;
        float wx = side == 0 ? posy + perp * rdy : posx + perp * rdx;
        wx -= floorf(wx);
        int tx = (int)(wx * TEX);
        if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) tx = TEX - 1 - tx;
        const color_t *col = tex[cell - 1] + tx;
        int s = 32 - (int)(perp * 2.2f);
        if (side) s = s * 3 / 4;
        if (s < 3) s = 3;
        uint32_t step = (uint32_t)((TEX << 16) / (h > 0 ? h : 1));
        int ys = y0 < 0 ? 0 : y0, ye = y1 > VIEW_H ? VIEW_H : y1;
        uint32_t ty = (uint32_t)(ys - y0) * step;
        color_t *p = fb + ys * SCREEN_W + x;
        for (int y = ys; y < ye; y++, p += SCREEN_W, ty += step) *p = shade(col[((ty >> 16) & (TEX - 1)) * TEX], s);

        if (!floor_on) {
            for (int y = 0; y < ys; y++) fb[y * SCREEN_W + x] = RGB(40, 36, 48);
            for (int y = ye; y < VIEW_H; y++) fb[y * SCREEN_W + x] = RGB(70, 62, 50);
            continue;
        }
        /* floor and ceiling: row_dist[y] is the distance seen at row y; mirror it for the ceiling */
        for (int y = ye; y < VIEW_H; y++) {
            float d = row_dist[y];
            float fx = posx + rdx * d, fy = posy + rdy * d;
            int u = (int)(fx * TEX) & (TEX - 1), v = (int)(fy * TEX) & (TEX - 1);
            int fs = 32 - (int)(d * 2.6f);
            if (fs < 2) fs = 2;
            fb[y * SCREEN_W + x] = shade(tex[T_FLOOR][v * TEX + u], fs);
            fb[(VIEW_H - 1 - y) * SCREEN_W + x] = shade(tex[T_CEIL][v * TEX + u], fs);
        }
    }
}

static int by_distance(const void *a, const void *b)
{
    float da = (*(Sprite *const *)a)->dist, db = (*(Sprite *const *)b)->dist;
    return da < db ? 1 : da > db ? -1 : 0;   /* farthest first */
}

static void draw_sprites(color_t *fb, float t)
{
    int n = 0;
    for (int i = 0; i < nsprites; i++) {
        Sprite *s = &sprites[i];
        if (!s->alive) continue;
        float dx = s->x - posx, dy = s->y - posy;
        s->dist = dx * dx + dy * dy;
        order[n++] = s;
    }
    qsort(order, (size_t)n, sizeof order[0], by_distance);
    float inv = 1.0f / (planex * diry - dirx * planey);
    for (int i = 0; i < n; i++) {
        Sprite *s = order[i];
        float dx = s->x - posx, dy = s->y - posy;
        float tx = inv * (diry * dx - dirx * dy);
        float ty = inv * (-planey * dx + planex * dy);
        if (ty < 0.15f) continue;                       /* behind the camera */
        float z = s->z;
        if (s->tex == T_ORB) z += 0.06f * sinf(t * 4.0f + s->x);
        int cx = (int)(VIEW_W / 2 * (1.0f + tx / ty));
        int size = (int)(PROJ * s->scale / ty);
        if (size < 1) continue;
        int cy = VIEW_H / 2 + (int)((0.5f - z) * PROJ / ty);
        int x0 = cx - size / 2, y0 = cy - size / 2;
        int sh = 32 - (int)(ty * 2.2f);
        if (s->tex == T_ORB) sh = 32;                   /* glowing: no fog */
        if (sh < 3) sh = 3;
        const color_t *img = tex[s->tex];
        uint32_t step = (uint32_t)((TEX << 16) / size);
        for (int x = x0 < 0 ? 0 : x0; x < x0 + size && x < VIEW_W; x++) {
            if (ty >= zbuf[x]) continue;
            int u = (int)(((uint32_t)(x - x0) * step) >> 16);
            int ys = y0 < 0 ? 0 : y0, ye = y0 + size > VIEW_H ? VIEW_H : y0 + size;
            uint32_t v = (uint32_t)(ys - y0) * step;
            for (int y = ys; y < ye; y++, v += step) {
                color_t c = img[(v >> 16) * TEX + u];
                if (c) fb[y * SCREEN_W + x] = shade(c, sh);
            }
        }
    }
}

static void draw_hud(void)
{
    const int cs = 4, mx0 = 8, my0 = VIEW_H + 12;
    gfx_fill_rect(0, VIEW_H, SCREEN_W, SCREEN_H - VIEW_H, RGB(20, 20, 28));
    gfx_hline(0, VIEW_H, SCREEN_W, GRAY);
    static const color_t wall_col[5] = { 0, RGB(170, 60, 50), RGB(130, 130, 140), RGB(150, 100, 50), RGB(80, 110, 170) };
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            int c = map[y][x];
            color_t col = c == PILLAR ? LIGHTGRAY : c ? wall_col[c] : RGB(35, 35, 45);
            gfx_fill_rect(mx0 + x * cs, my0 + y * cs, cs, cs, col);
        }
    for (int i = 0; i < nsprites; i++)
        if (sprites[i].alive && sprites[i].tex == T_ORB)
            gfx_fill_rect(mx0 + (int)(sprites[i].x * cs) - 1, my0 + (int)(sprites[i].y * cs) - 1, 3, 3, YELLOW);
    int px = mx0 + (int)(posx * cs), py = my0 + (int)(posy * cs);
    /* view cone */
    gfx_line(px, py, px + (int)((dirx - planex) * 14), py + (int)((diry - planey) * 14), DARKGREEN);
    gfx_line(px, py, px + (int)((dirx + planex) * 14), py + (int)((diry + planey) * 14), DARKGREEN);
    gfx_fill_circle(px, py, 2, GREEN);

    int tx = mx0 + MAP_W * cs + 16, ty = VIEW_H + 12;
    gfx_font(FONT_7X13);
    gfx_text(tx, ty, "RAYCASTER", WHITE);
    gfx_font(FONT_6X10);
    uint32_t secs = ((finish_ms ? finish_ms : sys_millis()) - start_ms) / 1000;
    gfx_printf(tx, ty + 20, YELLOW, "orbs  %d / %d", orbs_total - orbs_left, orbs_total);
    gfx_printf(tx, ty + 32, LIGHTGRAY, "time  %u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
    gfx_printf(tx, ty + 44, LIGHTGRAY, "fps   %d", fps);
    gfx_printf(tx, ty + 56, LIGHTGRAY, "floor %s", floor_on ? "on" : "off");
    gfx_text(tx, ty + 76, "arrows walk/turn", GRAY);
    gfx_text(tx, ty + 86, "z x strafe  f floor", GRAY);
    gfx_text(tx, ty + 96, "ESC quit", GRAY);
}

static void draw(float t)
{
    color_t *fb = gfx_framebuffer();
    draw_walls(fb);
    draw_sprites(fb, t);
    draw_hud();
    if (finish_ms) {
        gfx_fill_round_rect(60, 70, 200, 56, 8, RGB(10, 40, 20));
        gfx_round_rect(60, 70, 200, 56, 8, GREEN);
        gfx_font(FONT_7X13);
        gfx_text(160 - gfx_text_width("MAZE CLEARED") / 2, 80, "MAZE CLEARED", WHITE);
        gfx_font(FONT_6X10);
        gfx_text(160 - gfx_text_width("ENTER to play again") / 2, 104, "ENTER to play again", LIGHTGRAY);
    }
    gfx_flush();
}

void setup(void)
{
    make_textures();
    for (int y = VIEW_H / 2 + 1; y < VIEW_H; y++) row_dist[y] = 0.5f * PROJ / ((float)y - VIEW_H / 2 + 0.5f);
    reset_world();
    last_ms = fps_ms = sys_millis();
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) {
        for (int t = 0; t < NTEX; t++) free(tex[t]);
        sys_exit();
    }
    if (k == 'f') floor_on = !floor_on;
    if (k == KEY_ENTER && finish_ms) reset_world();

    uint32_t now = sys_millis();
    float dt = (float)(now - last_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;
    update(dt);
    draw((float)(now - start_ms) / 1000.0f);

    frames++;
    if (now - fps_ms >= 1000) { fps = frames; frames = 0; fps_ms = now; }
    sys_yield();
}
