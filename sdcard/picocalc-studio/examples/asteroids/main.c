/* asteroids - a vector arcade game.
 * Everything is drawn with lines, including the text (a small stroke font). The screen is not
 * cleared between frames but halved, which leaves short phosphor trails like a vector monitor.
 * Physics runs at a fixed 60 steps per second whatever the frame rate; the sound is a small
 * software mixer (shots, explosions, thrust rumble, saucer warble, the heartbeat) streamed
 * with snd_play() just ahead of the clock.
 *
 * LEFT/RIGHT turn  UP thrust  SPACE/z fire  DOWN hyperspace  p pause  g glow  m mute  ESC quit */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 320
#define STEP_MS 16
#define PI 3.14159265f
#define HISCORE_FILE SD_ROOT "/picocalc-studio/asteroids.hi"

#define MAX_ROCKS 40
#define MAX_SHOTS 16
#define MAX_PARTS 220
#define MAX_RINGS 8
#define ROCK_NV 11

typedef struct { float x, y, vx, vy; } Body;

typedef struct {
    Body b;
    float ang, spin, r;
    float shape[ROCK_NV];        /* radius factor per vertex */
    int size;                    /* 3 large, 2 medium, 1 small, 0 = free slot */
} Rock;

typedef struct { Body b; int life; bool enemy; } Shot;
typedef struct { float x, y, vx, vy; int life, max; color_t c; } Part;
typedef struct { float x, y, r, vr; int life; color_t c; } Ring;
typedef struct { float x, y, vx, vy, ang, spin, len; } Debris;

typedef struct {
    Body b;
    bool alive, small;
    int fire_t, turn_t;
} Saucer;

enum { ST_TITLE, ST_PLAY, ST_OVER };

static Rock rocks[MAX_ROCKS];
static Shot shots[MAX_SHOTS];
static Part parts[MAX_PARTS];
static Ring rings[MAX_RINGS];
static Debris debris[6];
static Saucer ufo;
static Body ship;
static float ship_ang;
static bool ship_alive, thrusting;
static int respawn_t, invuln_t, fire_cd, debris_t;
static int state, level, lives, score, hiscore, next_life;
static int level_t, saucer_t, beat_t, beat_hi, over_t, shake;
static bool new_hi, paused, glow = true, muted;
static uint32_t last_ms, t0;
static int16_t stars[60][2];

static float frand(float a, float b) { return a + (b - a) * (float)(sys_random() & 0xFFFF) / 65535.0f; }
static float wrapd(float d, float size) { return d > size / 2 ? d - size : d < -size / 2 ? d + size : d; }
static void wrap(Body *b)
{
    if (b->x < 0) b->x += W; else if (b->x >= W) b->x -= W;
    if (b->y < 0) b->y += H; else if (b->y >= H) b->y -= H;
}
static bool hit(float ax, float ay, float bx, float by, float r)
{
    float dx = wrapd(ax - bx, W), dy = wrapd(ay - by, H);
    return dx * dx + dy * dy < r * r;
}

/* ===================================================================== sound */

#define RATE 11025
#define LEAD_SAMPLES (RATE / 10)
#define BLOCK 256
#define NVOICES 8

enum { V_OFF, V_PEW, V_BOOM, V_BEAT, V_BLIP };

typedef struct {
    int kind;
    int pos, len;
    float f, k, amp, phase, lp;
} Voice;

static Voice voices[NVOICES];
static int16_t audio[BLOCK];
static uint32_t sent;
static uint32_t noise_seed = 1;
static float thrust_amp, thrust_lp, ufo_amp, ufo_phase, ufo_lfo;

static float noise(void)
{
    noise_seed = noise_seed * 1664525u + 1013904223u;
    return (float)(int32_t)noise_seed * (1.0f / 2147483648.0f);
}

/* f0 sweeps exponentially to f1 over ms */
static void sfx(int kind, float f0, float f1, int ms, float amp)
{
    int best = 0;
    for (int i = 0; i < NVOICES; i++) {
        if (voices[i].kind == V_OFF) { best = i; break; }
        if (voices[i].pos * voices[best].len > voices[best].pos * voices[i].len) best = i;   /* most finished */
    }
    Voice *v = &voices[best];
    v->kind = kind;
    v->pos = 0;
    v->len = RATE * ms / 1000;
    v->f = f0;
    v->k = powf(f1 / f0, 1.0f / (float)v->len);
    v->amp = amp;
    v->phase = v->lp = 0;
}

static void synth(int16_t *out, int n)
{
    float ufo_target = ufo.alive && state == ST_PLAY ? 0.10f : 0;
    float thrust_target = thrusting && ship_alive ? 0.35f : 0;
    float ufo_base = ufo.small ? 900.0f : 420.0f;
    for (int i = 0; i < n; i++) {
        float s = 0;
        for (int j = 0; j < NVOICES; j++) {
            Voice *v = &voices[j];
            if (v->kind == V_OFF) continue;
            float env = 1.0f - (float)v->pos / (float)v->len;
            v->phase += v->f / RATE;
            if (v->phase >= 1) v->phase -= 1;
            float x;
            switch (v->kind) {
            case V_PEW: x = v->phase < 0.5f ? 1.0f : -1.0f; break;
            case V_BOOM:   /* noise through a low-pass that closes as it decays */
                v->lp += (noise() - v->lp) * (0.04f + 0.3f * env * env);
                x = v->lp * 2.5f;
                break;
            case V_BEAT:   /* a soft low thump: low-passed square */
                v->lp += ((v->phase < 0.5f ? 1.0f : -1.0f) - v->lp) * 0.08f;
                x = v->lp;
                break;
            default: x = v->phase < 0.25f ? 1.0f : -0.33f; break;
            }
            s += x * v->amp * env;
            v->f *= v->k;
            if (++v->pos >= v->len) v->kind = V_OFF;
        }
        /* continuous voices glide toward their targets so they never click */
        thrust_amp += (thrust_target - thrust_amp) * 0.002f;
        thrust_lp += (noise() - thrust_lp) * 0.06f;
        s += thrust_lp * thrust_amp * 3.0f;
        ufo_amp += (ufo_target - ufo_amp) * 0.002f;
        if (ufo_amp > 0.001f) {
            ufo_lfo += 7.0f / RATE;
            if (ufo_lfo >= 1) ufo_lfo -= 1;
            float tri = ufo_lfo < 0.5f ? ufo_lfo * 4 - 1 : 3 - ufo_lfo * 4;
            ufo_phase += ufo_base * (1.0f + 0.25f * tri) / RATE;
            if (ufo_phase >= 1) ufo_phase -= 1;
            s += (ufo_phase < 0.5f ? 1.0f : -1.0f) * ufo_amp;
        }
        if (muted) s = 0;
        int v = (int)(s * 14000.0f);
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

/* ============================================================ stroke drawing */

static int ox, oy;   /* screen shake offset */

/* a closed or open polyline given as offsets from (cx, cy); drawn again across the edges it overlaps */
static void draw_shape(const float *px, const float *py, int n, float cx, float cy, float r, bool closed, color_t c)
{
    int xs[3] = { 0 }, ys[3] = { 0 }, nx = 1, ny = 1;
    if (cx + r >= W) xs[nx++] = -W;
    if (cx - r < 0) xs[nx++] = W;
    if (cy + r >= H) ys[ny++] = -H;
    if (cy - r < 0) ys[ny++] = H;
    for (int a = 0; a < nx; a++)
        for (int b = 0; b < ny; b++) {
            float bx = cx + (float)(xs[a] + ox), by = cy + (float)(ys[b] + oy);
            for (int i = 0; i < n - 1 + closed; i++) {
                int j = (i + 1) % n;
                gfx_line((int)(bx + px[i]), (int)(by + py[i]), (int)(bx + px[j]), (int)(by + py[j]), c);
            }
        }
}

/* Stroke font on a 4x6 grid: each glyph is polylines of "xy" digit pairs, a space lifts the pen. */
static const char *const glyph_digits[10] = {
    "0040460600 0640", "102026 1636", "004043030646", "00404606 0343", "000343 4046",
    "400003434606", "400006464303", "004046", "0040460600 0343", "430300404606",
};
static const char *const glyph_alpha[26] = {
    "06004046 0343", "06003041423303 3344453606", "40000646", "06003041453606", "40000646 0333",
    "400006 0333", "400006464323", "0006 4046 0343", "0040 2026 0646", "4045361605",
    "0006 400346", "000646", "0600234046", "06004640", "0040460600",
    "0600404303", "0040460600 2446", "0600404303 1346", "400003434606", "0040 2026",
    "00064640", "002640", "0006234640", "0046 4006", "002340 2326", "00400646",
};

static float text_width(const char *s, float sc) { return (float)strlen(s) * 6 * sc - 2 * sc; }

static void vtext(float x, float y, const char *s, float sc, color_t c)
{
    for (; *s; s++, x += 6 * sc) {
        const char *g = NULL;
        if (*s >= '0' && *s <= '9') g = glyph_digits[*s - '0'];
        else if (*s >= 'A' && *s <= 'Z') g = glyph_alpha[*s - 'A'];
        else if (*s == '-') g = "0343";
        if (!g) continue;
        int px = -1, py = -1;
        for (; *g; ) {
            if (*g == ' ') { px = -1; g++; continue; }
            int gx = g[0] - '0', gy = g[1] - '0';
            g += 2;
            if (px >= 0)
                gfx_line((int)(x + (float)px * sc) + ox, (int)(y + (float)py * sc) + oy,
                         (int)(x + (float)gx * sc) + ox, (int)(y + (float)gy * sc) + oy, c);
            px = gx; py = gy;
        }
    }
}

static void vtext_center(float y, const char *s, float sc, color_t c) { vtext((W - text_width(s, sc)) / 2, y, s, sc, c); }

/* ============================================================ game objects */

static const float ship_px[4] = { 11.0f, -7.0f, -4.0f, -7.0f }, ship_py[4] = { 0.0f, 6.0f, 0.0f, -6.0f };

static void rotate(const float *px, const float *py, int n, float ang, float sc, float *ox_, float *oy_)
{
    float c = cosf(ang), s = sinf(ang);
    for (int i = 0; i < n; i++) {
        ox_[i] = (px[i] * c - py[i] * s) * sc;
        oy_[i] = (px[i] * s + py[i] * c) * sc;
    }
}

static void add_parts(float x, float y, float vx, float vy, int n, float speed, color_t c)
{
    for (int i = 0, slot = 0; i < n; i++) {
        while (slot < MAX_PARTS && parts[slot].life > 0) slot++;
        if (slot == MAX_PARTS) return;
        Part *p = &parts[slot];
        float a = frand(0, 2 * PI), v = frand(0.2f, 1.0f) * speed;
        p->x = x; p->y = y;
        p->vx = vx * 0.5f + cosf(a) * v;
        p->vy = vy * 0.5f + sinf(a) * v;
        p->life = p->max = 20 + (int)(sys_random() % 30);
        p->c = c;
    }
}

static void add_ring(float x, float y, float vr, color_t c)
{
    for (int i = 0; i < MAX_RINGS; i++)
        if (rings[i].life <= 0) {
            rings[i] = (Ring){ x, y, 4, vr, 24, c };
            return;
        }
}

static Rock *spawn_rock(float x, float y, int size, float speed)
{
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &rocks[i];
        if (r->size) continue;
        float a = frand(0, 2 * PI);
        r->b = (Body){ x, y, cosf(a) * speed, sinf(a) * speed };
        r->size = size;
        r->r = size == 3 ? 30 : size == 2 ? 16 : 8;
        r->ang = frand(0, 2 * PI);
        r->spin = frand(-0.04f, 0.04f);
        for (int v = 0; v < ROCK_NV; v++) r->shape[v] = frand(0.72f, 1.05f);
        return r;
    }
    return NULL;
}

static int rocks_alive(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ROCKS; i++) n += rocks[i].size != 0;
    return n;
}

static void add_score(int pts)
{
    if (state != ST_PLAY) return;
    score += pts;
    if (score >= next_life) {
        lives++;
        next_life += 10000;
        sfx(V_BLIP, 880, 1760, 300, 0.25f);
    }
}

static void break_rock(Rock *r, float hvx, float hvy, bool award)
{
    static const int pts[4] = { 0, 100, 50, 20 };
    if (award) add_score(pts[r->size]);
    add_parts(r->b.x, r->b.y, r->b.vx, r->b.vy, r->size * 10, 1.0f + 0.4f * (float)r->size, RGB(200, 190, 170));
    sfx(V_BOOM, 1, 1, 180 + 160 * r->size, 0.20f + 0.08f * (float)r->size);
    if (r->size == 3) add_ring(r->b.x, r->b.y, 1.6f, RGB(120, 110, 100));
    int size = r->size - 1;
    float x = r->b.x, y = r->b.y, vx = r->b.vx + hvx * 0.15f, vy = r->b.vy + hvy * 0.15f;
    r->size = 0;
    if (!size) return;
    for (int i = 0; i < 2; i++) {
        Rock *c = spawn_rock(x, y, size, frand(0.6f, 1.4f) + 0.15f * (float)level);
        if (c) { c->b.vx += vx * 0.6f; c->b.vy += vy * 0.6f; }
    }
}

static void fire(float x, float y, float ang, float speed, Body *from, bool enemy)
{
    for (int i = 0; i < MAX_SHOTS; i++)
        if (shots[i].life <= 0) {
            Shot *s = &shots[i];
            s->b = (Body){ x, y, cosf(ang) * speed + (from ? from->vx : 0), sinf(ang) * speed + (from ? from->vy : 0) };
            s->life = enemy ? 70 : 52;
            s->enemy = enemy;
            sfx(V_PEW, enemy ? 900 : 1800, enemy ? 300 : 350, enemy ? 160 : 120, 0.12f);
            return;
        }
}

static int player_shots(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SHOTS; i++) n += shots[i].life > 0 && !shots[i].enemy;
    return n;
}

static void kill_ship(void)
{
    ship_alive = false;
    respawn_t = 120;
    shake = 14;
    add_parts(ship.x, ship.y, ship.vx, ship.vy, 40, 2.2f, RGB(120, 220, 255));
    add_parts(ship.x, ship.y, ship.vx, ship.vy, 20, 1.2f, RGB(255, 220, 80));
    add_ring(ship.x, ship.y, 2.4f, RGB(80, 160, 220));
    sfx(V_BOOM, 1, 1, 1100, 0.6f);
    float px[4], py[4];
    rotate(ship_px, ship_py, 4, ship_ang, 1, px, py);
    for (int i = 0; i < 4; i++) {   /* the hull's four edges fly apart */
        int j = (i + 1) % 4;
        Debris *d = &debris[i];
        float mx = (px[i] + px[j]) / 2, my = (py[i] + py[j]) / 2;
        d->x = ship.x + mx; d->y = ship.y + my;
        d->vx = ship.vx * 0.4f + mx * 0.06f + frand(-0.3f, 0.3f);
        d->vy = ship.vy * 0.4f + my * 0.06f + frand(-0.3f, 0.3f);
        d->ang = atan2f(py[j] - py[i], px[j] - px[i]);
        d->spin = frand(-0.15f, 0.15f);
        d->len = sqrtf((px[j] - px[i]) * (px[j] - px[i]) + (py[j] - py[i]) * (py[j] - py[i]));
    }
    debris_t = 90;
    if (--lives <= 0) {
        state = ST_OVER;
        over_t = 0;
        if (score > hiscore) {
            hiscore = score;
            new_hi = true;
            FILE *f = fopen(HISCORE_FILE, "w");
            if (f) { fprintf(f, "%d\n", hiscore); fclose(f); }
        }
    }
}

static void place_ship(void)
{
    ship = (Body){ W / 2, H / 2, 0, 0 };
    ship_ang = -PI / 2;
    ship_alive = true;
    invuln_t = 150;
}

static void start_level(void)
{
    level++;
    int n = 3 + level < 11 ? 3 + level : 11;
    for (int i = 0; i < n; i++) {
        /* start on the edges, away from the ship */
        float x, y;
        if (sys_random() & 1) { x = frand(0, W); y = 0; }
        else { x = 0; y = frand(0, H); }
        spawn_rock(x, y, 3, frand(0.4f, 0.9f) + 0.1f * (float)level);
    }
    saucer_t = 900 + (int)(sys_random() % 600);
    level_t = 0;
}

static void new_game(void)
{
    memset(rocks, 0, sizeof rocks);
    memset(shots, 0, sizeof shots);
    ufo.alive = false;
    level = 0;
    score = 0;
    lives = 3;
    next_life = 10000;
    new_hi = false;
    state = ST_PLAY;
    place_ship();
    start_level();
}

/* -------------------------------------------------------------- the saucer */

static void spawn_saucer(void)
{
    ufo.alive = true;
    ufo.small = score >= 10000 || (level >= 3 && (sys_random() & 1));
    float speed = ufo.small ? 1.6f : 1.1f;
    bool from_left = sys_random() & 1;
    ufo.b = (Body){ from_left ? 0.0f : W - 1.0f, frand(40, H - 40), from_left ? speed : -speed, 0 };
    ufo.fire_t = 60;
    ufo.turn_t = 60;
}

static void destroy_saucer(bool by_player)
{
    if (by_player) add_score(ufo.small ? 1000 : 200);
    add_parts(ufo.b.x, ufo.b.y, ufo.b.vx, ufo.b.vy, 36, 2.0f, RGB(255, 90, 220));
    add_ring(ufo.b.x, ufo.b.y, 2.0f, RGB(200, 60, 180));
    sfx(V_BOOM, 1, 1, 700, 0.45f);
    ufo.alive = false;
    saucer_t = 700 + (int)(sys_random() % 700);
}

static void step_saucer(void)
{
    if (!ufo.alive) {
        if (state == ST_PLAY && --saucer_t <= 0) spawn_saucer();
        return;
    }
    ufo.b.x += ufo.b.vx;
    ufo.b.y += ufo.b.vy;
    if (ufo.b.x < -20 || ufo.b.x > W + 20) {   /* crossed the screen: leave */
        ufo.alive = false;
        saucer_t = 600 + (int)(sys_random() % 600);
        return;
    }
    if (ufo.b.y < 0) ufo.b.y += H; else if (ufo.b.y >= H) ufo.b.y -= H;
    if (--ufo.turn_t <= 0) {
        ufo.b.vy = (float)((int)(sys_random() % 3) - 1) * fabsf(ufo.b.vx) * 0.8f;
        ufo.turn_t = 50 + (int)(sys_random() % 70);
    }
    if (--ufo.fire_t <= 0) {
        float ang;
        if (ufo.small && ship_alive) {   /* aimed, and better as the score grows */
            float err = score > 30000 ? 0.05f : score > 15000 ? 0.15f : 0.3f;
            ang = atan2f(wrapd(ship.y - ufo.b.y, H), wrapd(ship.x - ufo.b.x, W)) + frand(-err, err);
        } else {
            ang = frand(0, 2 * PI);
        }
        fire(ufo.b.x, ufo.b.y, ang, 3.5f, NULL, true);
        ufo.fire_t = ufo.small ? 40 : 60;
    }
}

/* ================================================================= stepping */

static void step(void)
{
    /* ship */
    thrusting = false;
    if (state == ST_PLAY && ship_alive) {
        if (kbd_down(KEY_LEFT)) ship_ang -= 0.075f;
        if (kbd_down(KEY_RIGHT)) ship_ang += 0.075f;
        if (kbd_down(KEY_UP)) {
            thrusting = true;
            ship.vx += cosf(ship_ang) * 0.11f;
            ship.vy += sinf(ship_ang) * 0.11f;
            if ((sys_random() & 3) == 0) {   /* exhaust */
                for (int i = 0; i < MAX_PARTS; i++)
                    if (parts[i].life <= 0) {
                        float a = ship_ang + PI + frand(-0.3f, 0.3f);
                        parts[i] = (Part){ ship.x - cosf(ship_ang) * 6, ship.y - sinf(ship_ang) * 6,
                                           ship.vx + cosf(a) * 1.5f, ship.vy + sinf(a) * 1.5f, 12, 12, RGB(255, 160, 40) };
                        break;
                    }
            }
        }
        float sp = sqrtf(ship.vx * ship.vx + ship.vy * ship.vy);
        if (sp > 5.5f) { ship.vx *= 5.5f / sp; ship.vy *= 5.5f / sp; }
        ship.vx *= 0.991f;
        ship.vy *= 0.991f;
        ship.x += ship.vx;
        ship.y += ship.vy;
        wrap(&ship);
        if (invuln_t > 0) invuln_t--;
        if (fire_cd > 0) fire_cd--;
    } else if (state == ST_PLAY && --respawn_t <= 0) {
        /* respawn once the middle is clear (or after a while, protected) */
        bool clear = true;
        for (int i = 0; i < MAX_ROCKS; i++)
            if (rocks[i].size && hit(rocks[i].b.x, rocks[i].b.y, W / 2, H / 2, rocks[i].r + 50)) clear = false;
        if (clear || respawn_t < -240) place_ship();
    }

    /* rocks */
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &rocks[i];
        if (!r->size) continue;
        r->b.x += r->b.vx;
        r->b.y += r->b.vy;
        r->ang += r->spin;
        wrap(&r->b);
    }

    step_saucer();

    /* shots against everything */
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &shots[i];
        if (s->life <= 0) continue;
        s->b.x += s->b.vx;
        s->b.y += s->b.vy;
        wrap(&s->b);
        s->life--;
        for (int j = 0; j < MAX_ROCKS && s->life > 0; j++) {
            Rock *r = &rocks[j];
            if (r->size && hit(s->b.x, s->b.y, r->b.x, r->b.y, r->r * 0.92f)) {
                break_rock(r, s->b.vx, s->b.vy, !s->enemy);
                s->life = 0;
            }
        }
        if (s->life <= 0) continue;
        if (!s->enemy && ufo.alive && hit(s->b.x, s->b.y, ufo.b.x, ufo.b.y, ufo.small ? 7 : 12)) {
            destroy_saucer(true);
            s->life = 0;
        } else if (s->enemy && ship_alive && state == ST_PLAY && invuln_t == 0 && hit(s->b.x, s->b.y, ship.x, ship.y, 8)) {
            s->life = 0;
            kill_ship();
        }
    }

    /* bodies against rocks */
    for (int j = 0; j < MAX_ROCKS; j++) {
        Rock *r = &rocks[j];
        if (!r->size) continue;
        if (state == ST_PLAY && ship_alive && invuln_t == 0 && hit(ship.x, ship.y, r->b.x, r->b.y, r->r * 0.85f + 6)) {
            break_rock(r, ship.vx, ship.vy, true);
            kill_ship();
        } else if (ufo.alive && hit(ufo.b.x, ufo.b.y, r->b.x, r->b.y, r->r * 0.85f + (ufo.small ? 6 : 11))) {
            break_rock(r, ufo.b.vx, ufo.b.vy, false);   /* rocks the saucer hits score nothing */
            destroy_saucer(false);
        }
    }
    if (state == ST_PLAY && ship_alive && invuln_t == 0 && ufo.alive && hit(ship.x, ship.y, ufo.b.x, ufo.b.y, ufo.small ? 12 : 17)) {
        destroy_saucer(true);
        kill_ship();
    }

    /* effects */
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &parts[i];
        if (p->life <= 0) continue;
        p->x += p->vx; p->y += p->vy;
        p->vx *= 0.97f; p->vy *= 0.97f;
        p->life--;
    }
    for (int i = 0; i < MAX_RINGS; i++)
        if (rings[i].life > 0) { rings[i].r += rings[i].vr; rings[i].life--; }
    if (debris_t > 0) {
        debris_t--;
        for (int i = 0; i < 4; i++) {
            Debris *d = &debris[i];
            d->x += d->vx; d->y += d->vy; d->ang += d->spin;
        }
    }
    if (shake > 0) shake--;

    /* the heartbeat speeds up as the field empties */
    if (state == ST_PLAY && rocks_alive()) {
        int n = rocks_alive();
        int period = 18 + n * 3 > 60 ? 60 : 18 + n * 3;
        if (++beat_t >= period) {
            beat_t = 0;
            beat_hi ^= 1;
            sfx(V_BEAT, beat_hi ? 62 : 55, beat_hi ? 58 : 50, 110, 0.5f);
        }
    }

    /* next level once everything is gone */
    if (state == ST_PLAY && !rocks_alive() && !ufo.alive && ++level_t > 120) start_level();
    if (state == ST_OVER) over_t++;
    if (state == ST_TITLE && rocks_alive() < 4) spawn_rock(0, frand(0, H), 3, frand(0.4f, 0.8f));
}

/* =================================================================== drawing */

static void draw_ship(float x, float y, float ang, float sc, color_t c, bool flame)
{
    float px[4], py[4];
    rotate(ship_px, ship_py, 4, ang, sc, px, py);
    draw_shape(px, py, 4, x, y, 12 * sc, true, c);
    if (flame) {
        float fx[3] = { -5, -7 - frand(4, 10), -5 }, fy[3] = { -3, 0, 3 }, rx[3], ry[3];
        rotate(fx, fy, 3, ang, sc, rx, ry);
        draw_shape(rx, ry, 3, x, y, 20 * sc, false, RGB(255, 170, 60));
    }
}

static void draw_saucer(void)
{
    static const float sx[] = { -12.0f, -5.0f, 5.0f, 12.0f, 5.0f, -5.0f, -12.0f, 12.0f, -5.0f, -3.0f, 3.0f, 5.0f };
    static const float sy[] = { 0.0f, 4.0f, 4.0f, 0.0f, -4.0f, -4.0f, 0.0f, 0.0f, -4.0f, -8.0f, -8.0f, -4.0f };
    float sc = ufo.small ? 0.6f : 1.0f, px[12], py[12];
    for (int i = 0; i < 12; i++) { px[i] = sx[i] * sc; py[i] = sy[i] * sc; }
    color_t c = RGB(255, 110, 230);
    draw_shape(px, py, 6, ufo.b.x, ufo.b.y, 13, true, c);        /* hull */
    draw_shape(px + 6, py + 6, 2, ufo.b.x, ufo.b.y, 13, false, c);  /* rim */
    draw_shape(px + 8, py + 8, 4, ufo.b.x, ufo.b.y, 13, false, c);  /* dome */
}

static void draw_world(void)
{
    color_t *fb = gfx_framebuffer();
    if (glow) {
        for (int i = 0; i < W * H; i++) fb[i] = (color_t)((fb[i] >> 1) & 0x7BEF);   /* phosphor decay */
    } else {
        gfx_clear(BLACK);
    }
    for (int i = 0; i < 60; i++) gfx_pixel(stars[i][0], stars[i][1], i & 3 ? RGB(50, 50, 70) : RGB(110, 110, 140));

    ox = shake ? (int)(sys_random() % (unsigned)(shake + 1)) - shake / 2 : 0;
    oy = shake ? (int)(sys_random() % (unsigned)(shake + 1)) - shake / 2 : 0;

    for (int i = 0; i < MAX_RINGS; i++) {
        Ring *r = &rings[i];
        if (r->life > 0) gfx_circle((int)r->x + ox, (int)r->y + oy, (int)r->r, r->life > 8 ? r->c : RGB(50, 50, 50));
    }
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &parts[i];
        if (p->life <= 0) continue;
        int s = p->life * 32 / p->max;
        color_t c = p->c;
        uint32_t rb = c & 0xF81F, g = c & 0x07E0;
        c = (color_t)(((rb * (uint32_t)s >> 5) & 0xF81F) | ((g * (uint32_t)s >> 5) & 0x07E0));
        gfx_pixel((int)p->x + ox, (int)p->y + oy, c);
        if (p->life > p->max / 2) gfx_pixel((int)p->x + ox + 1, (int)p->y + oy, c);
    }
    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &rocks[i];
        if (!r->size) continue;
        float px[ROCK_NV], py[ROCK_NV];
        for (int v = 0; v < ROCK_NV; v++) {
            float a = r->ang + (float)v * (2 * PI / ROCK_NV);
            px[v] = cosf(a) * r->r * r->shape[v];
            py[v] = sinf(a) * r->r * r->shape[v];
        }
        draw_shape(px, py, ROCK_NV, r->b.x, r->b.y, r->r * 1.05f, true, state == ST_TITLE ? RGB(150, 150, 160) : RGB(225, 220, 210));
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &shots[i];
        if (s->life <= 0) continue;
        color_t c = s->enemy ? RGB(255, 120, 220) : WHITE;
        int x = (int)s->b.x + ox, y = (int)s->b.y + oy;
        gfx_fill_rect(x - 1, y - 1, 2, 2, c);
    }
    if (ufo.alive) draw_saucer();
    if (debris_t > 0)
        for (int i = 0; i < 4; i++) {
            Debris *d = &debris[i];
            float c = cosf(d->ang) * d->len / 2, s = sinf(d->ang) * d->len / 2;
            float px[2] = { -c, c }, py[2] = { -s, s };
            draw_shape(px, py, 2, d->x, d->y, d->len, false, debris_t > 30 ? RGB(120, 220, 255) : RGB(40, 80, 100));
        }
    if (state == ST_PLAY && ship_alive && (invuln_t == 0 || (invuln_t / 6) & 1))
        draw_ship(ship.x, ship.y, ship_ang, 1, RGB(120, 230, 255), thrusting && (sys_random() & 1));
}

static void draw_hud(void)
{
    char buf[32];
    ox = oy = 0;
    snprintf(buf, sizeof buf, "%d", score);
    vtext(10, 8, buf, 2.5f, WHITE);
    snprintf(buf, sizeof buf, "%d", hiscore);
    vtext_center(8, buf, 1.5f, GRAY);
    for (int i = 0; i < lives && i < 8; i++) draw_ship(16 + (float)i * 14, 36, -PI / 2, 0.8f, RGB(120, 230, 255), false);
    snprintf(buf, sizeof buf, "%d", level);
    vtext(W - 10 - text_width(buf, 1.5f), 8, buf, 1.5f, GRAY);
    if (state == ST_PLAY && !rocks_alive() && !ufo.alive && level_t > 20) {
        snprintf(buf, sizeof buf, "LEVEL %d", level + 1);
        vtext_center(H / 2 - 30, buf, 3, YELLOW);
    }
    if (paused) vtext_center(H / 2 - 12, "PAUSED", 4, YELLOW);
}

static void draw_title(void)
{
    char buf[32];
    ox = oy = 0;
    uint32_t t = (sys_millis() - t0) / 16;
    float bob = sinf((float)t * 0.05f) * 3;
    vtext_center(70 + bob, "ASTEROIDS", 5, RGB(140, 230, 255));
    snprintf(buf, sizeof buf, "HIGH SCORE %d", hiscore);
    vtext_center(130, buf, 2, GRAY);
    if ((t / 30) & 1) vtext_center(180, "PRESS ENTER", 2.5f, YELLOW);
    static const char *const help[] = {
        "LEFT/RIGHT turn     UP thrust", "SPACE or z fire  DOWN hyperspace", "p pause  g glow  m mute  ESC quit",
    };
    for (int i = 0; i < 3; i++) gfx_text((W - gfx_text_width(help[i])) / 2, 236 + i * 14, help[i], RGB(110, 110, 130));
    gfx_text((W - gfx_text_width("rocks 20/50/100  saucers 200/1000")) / 2, 290, "rocks 20/50/100  saucers 200/1000", RGB(80, 80, 100));
}

static void draw_over(void)
{
    vtext_center(H / 2 - 40, "GAME OVER", 4, RGB(255, 90, 90));
    if (new_hi && (over_t / 20) & 1) vtext_center(H / 2 + 10, "NEW HIGH SCORE", 2, YELLOW);
    if (over_t > 90) vtext_center(H / 2 + 50, "PRESS ENTER", 2, GRAY);
}

/* ===================================================================== main */

static void hyperspace(void)
{
    if (!ship_alive || state != ST_PLAY) return;
    add_parts(ship.x, ship.y, 0, 0, 16, 1.5f, RGB(160, 120, 255));
    ship.x = frand(20, W - 20);
    ship.y = frand(20, H - 20);
    ship.vx = ship.vy = 0;
    add_parts(ship.x, ship.y, 0, 0, 16, 1.5f, RGB(160, 120, 255));
    sfx(V_PEW, 200, 2400, 250, 0.15f);
    if (sys_random() % 10 == 0) kill_ship();   /* it is not without risk */
}

void setup(void)
{
    FILE *f = fopen(HISCORE_FILE, "r");
    if (f) { if (fscanf(f, "%d", &hiscore) != 1) hiscore = 0; fclose(f); }
    sys_seed(sys_millis() ^ 0x5eed);
    for (int i = 0; i < 60; i++) { stars[i][0] = (int16_t)(sys_random() % W); stars[i][1] = (int16_t)(sys_random() % H); }
    state = ST_TITLE;
    for (int i = 0; i < 6; i++) spawn_rock(frand(0, W), frand(0, H), 1 + (int)(sys_random() % 3), frand(0.3f, 0.8f));
    snd_volume(6);
    gfx_clear(BLACK);
    t0 = last_ms = sys_millis();
}

void loop(void)
{
    int k;
    while ((k = kbd_get()) != 0) {
        if (k == KEY_ESC) { snd_stop(); sys_exit(); }
        if (k == 'g') glow = !glow;
        if (k == 'm') muted = !muted;
        if (state == ST_TITLE && k == KEY_ENTER) { new_game(); continue; }
        if (state == ST_OVER && over_t > 90 && k == KEY_ENTER) {
            state = ST_TITLE;
            memset(shots, 0, sizeof shots);
            ufo.alive = false;
            continue;
        }
        if (state != ST_PLAY) continue;
        if (k == 'p') paused = !paused;
        if (paused) continue;
        if ((k == ' ' || k == 'z') && ship_alive && fire_cd == 0 && player_shots() < 5) {
            fire(ship.x + cosf(ship_ang) * 11, ship.y + sinf(ship_ang) * 11, ship_ang, 6.5f, &ship, false);
            fire_cd = 5;
        }
        if (k == KEY_DOWN || k == 'h') hyperspace();
    }

    uint32_t now = sys_millis();
    if (paused) {
        last_ms = now;
        thrusting = false;
    } else {
        int steps = 0;
        while (now - last_ms >= STEP_MS && steps < 4) {
            step();
            last_ms += STEP_MS;
            steps++;
        }
        if (now - last_ms > STEP_MS * 4) last_ms = now;   /* far behind: drop time rather than spiral */
    }
    pump_audio(now - t0);

    draw_world();
    if (state == ST_TITLE) draw_title();
    else {
        draw_hud();
        if (state == ST_OVER) draw_over();
    }
    gfx_flush();
}
