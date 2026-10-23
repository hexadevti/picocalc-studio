/* demoscene - a classic multi-part demo synced to a chiptune that is synthesised in real time.
 * Parts (4 bars each): starfield + wobbling logo, plasma, tunnel, rotozoomer, fire. Below them:
 * copper bars, a per-pixel sine scroller and channel meters.
 * Effects run at half resolution through precomputed tables (sine, palettes, tunnel maps);
 * the font is captured from the 8x8 system font so the logo and scroller can be scaled.
 * The music is a 6-channel tracker (bass, arpeggio, lead, kick, snare, hat) mixed into a buffer and
 * streamed with snd_play() just ahead of the clock, so a slow frame never stalls the audio.
 *   n next part   m mute   ESC quit */
#include <picocalc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EW 160                  /* effects are computed at 160x110 and doubled to 320x220 */
#define EH 110
#define PANEL_Y (EH * 2)
#define TEXW 64
#define NPARTS 5

/* ----------------------------------------------------------------- music */

#define RATE 16000
#define BPM 125
#define SPR (RATE * 60 / (BPM * 4))     /* samples per row (a 16th note) */
#define ROWS_PER_BAR 16
#define BAR_MS (SPR * ROWS_PER_BAR * 1000 / RATE)
#define LEAD_SAMPLES (RATE / 8)         /* keep ~125 ms queued */
#define BLOCK 512

enum { CH_BASS, CH_ARP, CH_LEAD, CH_KICK, CH_SNARE, CH_HAT, NCH };
static const char *const ch_names[NCH] = { "bas", "arp", "led", "kck", "snr", "hat" };

static const uint8_t chord_root[4] = { 45, 41, 48, 43 };           /* Am F C G */
static const uint8_t chord_third[4] = { 3, 4, 4, 4 };
/* lead: one entry per row over 4 bars, 0 = hold, 1 = note off */
static const uint8_t melody[64] = {
    76, 0, 72, 0, 69, 0, 72, 0, 76, 0, 79, 0, 76, 0, 72, 1,
    77, 0, 72, 0, 69, 0, 72, 0, 77, 0, 81, 0, 79, 0, 77, 1,
    76, 0, 72, 0, 67, 0, 72, 0, 76, 0, 79, 0, 84, 0, 0, 1,
    83, 0, 79, 0, 74, 0, 79, 0, 83, 0, 86, 0, 83, 0, 79, 1,
};

typedef struct {
    uint32_t phase, inc;
    int32_t env;                /* 0..32767 */
    int shift;                  /* decay: env -= env >> shift each sample */
} Voice;

static Voice v[NCH];
static uint32_t note_inc[128];
static int16_t sine[256];
static uint32_t noise_state = 0xACE1u, sample_pos, kick_inc;
static int32_t bass_lp, last_noise;
static int16_t audio[BLOCK];
static bool muted;

static void trigger(int ch, int note, int shift)
{
    if (note) v[ch].inc = note_inc[note];
    v[ch].env = 32767;
    v[ch].shift = shift;
}

static void song_row(uint32_t row)
{
    uint32_t bar = row / ROWS_PER_BAR, r = row % ROWS_PER_BAR;
    uint32_t sbar = bar < 16 ? bar : 4 + (bar - 4) % 12;   /* intro once, then loop bars 4..15 */
    int c = (int)(bar % 4);
    int root = chord_root[c];
    if (r % 2 == 0) trigger(CH_BASS, root + (r % 4 == 2 ? 12 : 0), 12);
    if (r != 0 && r != 4 && r != 8 && r != 12) trigger(CH_HAT, 0, r % 4 == 2 ? 9 : 7);
    if (sbar >= 4) {
        if (r % 4 == 0) { trigger(CH_KICK, 0, 11); kick_inc = 180u * (0x1000000u / RATE) * 256u; }
        if (r == 4 || r == 12) trigger(CH_SNARE, 0, 10);
        static const int arp[4] = { 0, 0, 7, 12 };
        int tone = (r % 4 == 1) ? chord_third[c] : arp[r % 4];
        trigger(CH_ARP, root + 24 + tone + ((r / 4) & 1) * 12, 10);
    }
    if (sbar >= 8) {
        uint8_t m = melody[(bar % 4) * 16 + r];
        if (m == 1) v[CH_LEAD].shift = 9;
        else if (m) trigger(CH_LEAD, m, 14);
    }
}

static int32_t noise(void)
{
    noise_state ^= noise_state << 13; noise_state ^= noise_state >> 17; noise_state ^= noise_state << 5;
    return (int32_t)(noise_state & 0xFFFF) - 32768;
}

static void synth(int16_t *out, int n)
{
    for (int i = 0; i < n; i++, sample_pos++) {
        if (sample_pos % SPR == 0) song_row(sample_pos / SPR);
        for (int c = 0; c < NCH; c++) v[c].env -= v[c].env >> v[c].shift;
        int32_t mix = 0;
        Voice *p;

        p = &v[CH_BASS];                                    /* saw through a one-pole low-pass */
        p->phase += p->inc;
        bass_lp += (((int32_t)(p->phase >> 16) - 32768) - bass_lp) >> 2;
        mix += (bass_lp >> 2) * p->env >> 15;

        p = &v[CH_ARP];                                     /* 25% pulse */
        p->phase += p->inc;
        mix += ((p->phase >> 30) == 0 ? 2600 : -2600) * p->env >> 15;

        p = &v[CH_LEAD];                                    /* two detuned squares */
        p->phase += p->inc;
        uint32_t ph2 = p->phase + (sample_pos * (p->inc >> 9));
        mix += (((p->phase >> 31) ? 2200 : -2200) + ((ph2 >> 31) ? 1600 : -1600)) * p->env >> 15;

        p = &v[CH_KICK];                                    /* sine with falling pitch */
        p->phase += kick_inc;
        if (kick_inc > 45u * (0x1000000u / RATE) * 256u) kick_inc -= kick_inc >> 10;
        mix += sine[p->phase >> 24] * p->env >> 15;

        int32_t nz = noise();
        mix += (nz >> 2) * v[CH_SNARE].env >> 15;
        mix += ((nz - last_noise) >> 3) * v[CH_HAT].env >> 15;   /* high-passed noise */
        last_noise = nz;

        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)(muted ? 0 : mix);
    }
}

/* -------------------------------------------------------------- graphics */

static uint8_t font[95][8];               /* captured 8x8 glyphs for ' '..'~', bit 7 = left */
static int8_t sin8[256];
static color_t pal_plasma[256], pal_fire[256], tex_tunnel[TEXW * TEXW], tex_roto[TEXW * TEXW];
/* Tunnel tables for one quarter of a 320x220 field (the rest is its mirror image), seen through a
 * moving 160x110 window: depth and angle (0..32 for a quarter turn). The shade goes by distance
 * only, and is worked out a row at a time (tun_shade_row). */
static uint8_t *tun_d, *tun_a;
static uint8_t tun_s[EW];
static uint8_t heat[EH + 2][EW];
static color_t row_buf[EW];
static uint8_t logo[8][64];               /* "PICOCALC" as a 64x8 bitmap */
static float beat;                        /* 1 on each kick, decays to 0 */
static int part_offset;

typedef struct { float x, y, z; } Star;
static Star stars[220];

static int isin(int a) { return sin8[a & 255]; }

static color_t hsv(float h, float s, float val)
{
    h -= floorf(h);
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    r = r < 0 ? 0 : r > 1 ? 1 : r; g = g < 0 ? 0 : g > 1 ? 1 : g; b = b < 0 ? 0 : b > 1 ? 1 : b;
    r = ((r - 1) * s + 1) * val; g = ((g - 1) * s + 1) * val; b = ((b - 1) * s + 1) * val;
    return RGB((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static color_t shade(color_t c, int s)    /* s = 0..32 */
{
    uint32_t rb = c & 0xF81Fu, g = c & 0x07E0u;
    return (color_t)((((rb * (uint32_t)s) >> 5) & 0xF81Fu) | (((g * (uint32_t)s) >> 5) & 0x07E0u));
}

/* write a half-resolution row as two full-resolution rows */
static void put_row(color_t *fb, int y, const color_t *row)
{
    color_t *d = fb + y * 2 * SCREEN_W;
    for (int x = 0; x < EW; x++) d[2 * x] = d[2 * x + 1] = row[x];
    memcpy(d + SCREEN_W, d, SCREEN_W * sizeof(color_t));
}

static void capture_font(void)
{
    gfx_font(FONT_8X8);
    for (int c = 0; c < 95; c++) {
        char s[2] = { (char)(c + 32), 0 };
        gfx_text_bg(0, 0, s, WHITE, BLACK);
        for (int y = 0; y < 8; y++) {
            uint8_t bits = 0;
            for (int x = 0; x < 8; x++) bits |= (uint8_t)((gfx_get_pixel(x, y) != BLACK) << (7 - x));
            font[c][y] = bits;
        }
    }
    gfx_font(FONT_6X10);
    const char *word = "PICOCALC";
    for (int i = 0; i < 8; i++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) logo[y][i * 8 + x] = (font[word[i] - 32][y] >> (7 - x)) & 1;
}

static void build_tables(void)
{
    for (int i = 0; i < 256; i++) {
        float a = (float)i * 6.2831853f / 256.0f;
        sin8[i] = (int8_t)(127.0f * sinf(a));
        sine[i] = (int16_t)(14000.0f * sinf(a));
        pal_plasma[i] = RGB((int)(128 + 127 * sinf(a)), (int)(128 + 127 * sinf(a * 2 + 2.0f)), (int)(128 + 127 * sinf(a + 4.0f)));
        int r = i * 3, g = i * 3 - 256, b = i * 4 - 640;      /* black - red - yellow - white */
        pal_fire[i] = RGB(r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g, b < 0 ? 0 : b > 255 ? 255 : b);
    }
    for (int n = 0; n < 128; n++)
        note_inc[n] = (uint32_t)(440.0 * pow(2.0, (n - 69) / 12.0) / RATE * 4294967296.0);
    for (int y = 0; y < TEXW; y++)
        for (int x = 0; x < TEXW; x++) {
            bool tile = ((x / 8) ^ (y / 8)) & 1;          /* tiles in both directions: no seam */
            tex_tunnel[y * TEXW + x] = hsv((float)y / TEXW, 0.6f, tile ? 1.0f : 0.45f);
            int dx = x % 32 - 16, dy = y % 32 - 16;
            bool ring = (dx * dx + dy * dy) / 48 % 2;
            bool check = ((x / 16) ^ (y / 16)) & 1;
            tex_roto[y * TEXW + x] = ring ? (check ? RGB(255, 200, 40) : RGB(240, 80, 40)) : (check ? RGB(30, 30, 90) : RGB(60, 40, 140));
        }
    tun_d = malloc(EW * EH);
    tun_a = malloc(EW * EH);
    if (!tun_d || !tun_a) {
        printf("demoscene: out of memory\n");
        sys_exit();
    }
    for (int y = 0; y < EH; y++)
        for (int x = 0; x < EW; x++) {
            float dx = (float)x, dy = (float)y;
            float r = sqrtf(dx * dx + dy * dy) + 0.5f;
            int i = y * EW + x;
            tun_d[i] = (uint8_t)((int)(1800.0f / r) & 63);
            tun_a[i] = (uint8_t)(atan2f(dy, dx) * (64.0f / 3.14159265f));
        }
    for (int i = 0; i < 220; i++) {
        stars[i].x = (float)((int)(sys_random() % 2000) - 1000);
        stars[i].y = (float)((int)(sys_random() % 2000) - 1000);
        stars[i].z = (float)(sys_random() % 1000 + 1);
    }
}

/* ---- parts: t = seconds since the part started */

static void part_stars(color_t *fb, float t)
{
    gfx_fill_rect(0, 0, SCREEN_W, PANEL_Y, BLACK);
    float speed = 6.0f + 30.0f * beat;
    for (int i = 0; i < 220; i++) {
        Star *s = &stars[i];
        s->z -= speed;
        if (s->z < 1) { s->z = 1000; s->x = (float)((int)(sys_random() % 2000) - 1000); s->y = (float)((int)(sys_random() % 2000) - 1000); }
        int x = 160 + (int)(s->x * 128.0f / s->z), y = PANEL_Y / 2 + (int)(s->y * 128.0f / s->z);
        if (x < 0 || x >= SCREEN_W || y < 0 || y >= PANEL_Y) continue;
        int b = 32 - (int)(s->z / 32.0f);
        color_t c = shade(WHITE, b < 2 ? 2 : b);
        fb[y * SCREEN_W + x] = c;
        if (s->z < 300 && x + 1 < SCREEN_W) fb[y * SCREEN_W + x + 1] = c;
    }
    /* logo: 64x8 bitmap scaled 4x, every row wobbling on its own sine */
    int y0 = 60;
    for (int y = 0; y < 8 * 4; y++) {
        int wob = isin(y * 5 + (int)(t * 300)) * 7 / 127;
        color_t c = hsv(t * 0.2f + (float)y / 64.0f, 0.7f, 1.0f);
        for (int x = 0; x < 64; x++)
            if (logo[y / 4][x]) gfx_fill_rect(32 + x * 4 + wob, y0 + y, 4, 1, c);
    }
    const char *sub = "S T U D I O";
    gfx_font(FONT_7X13);
    gfx_text(160 - gfx_text_width(sub) / 2, y0 + 48, sub, shade(WHITE, 16 + (int)(16 * beat)));
    gfx_font(FONT_6X10);
}

static void part_plasma(color_t *fb, float t)
{
    int t1 = (int)(t * 90), t2 = (int)(t * -70), t3 = (int)(t * 50), shift = (int)(t * 60) + (int)(beat * 40);
    for (int y = 0; y < EH; y++) {
        int sy = isin(y * 5 + t2);
        for (int x = 0; x < EW; x++) {
            int val = isin(x * 4 + t1) + sy + isin((x + y) * 2 + t3) + isin(((x * y) >> 5) - t1);
            row_buf[x] = pal_plasma[(((val + 512) >> 1) + shift) & 255];
        }
        put_row(fb, y, row_buf);
    }
}

/* tun_s[x] = min((int)((sqrt(x*x + y*y) + 0.5) / 3), 32), in integers: the shade reaches s at
 * x*x + y*y >= (3s - 0.5)^2, that is 9s^2 - 3s + 1. */
static void tun_shade_row(int y)
{
    int s = 0;
    for (int x = 0; x < EW; x++) {
        int q = x * x + y * y;
        while (s < 32 && q >= 9 * (s + 1) * (s + 1) - 3 * (s + 1) + 1) s++;
        tun_s[x] = (uint8_t)s;
    }
}

static void part_tunnel(color_t *fb, float t)
{
    int ox = EW / 2 + isin((int)(t * 40)) * (EW / 2 - 4) / 127;
    int oy = EH / 2 + isin((int)(t * 55) + 64) * (EH / 2 - 4) / 127;
    int move = (int)(t * 40.0f), turn = (int)(t * 12.0f);
    int split = EW - ox;                    /* window columns left of the field's centre */
    for (int y = 0; y < EH; y++) {
        int fy = y + oy, top = fy < EH;     /* the angle runs backwards in the mirrored quarters */
        int ry = top ? EH - fy : fy - EH;
        const uint8_t *d = tun_d + ry * EW, *a = tun_a + ry * EW, *s = tun_s;
        tun_shade_row(ry);
        for (int x = 0; x < split; x++) {
            int q = EW - ox - x, u = top ? a[q] : -a[q];
            row_buf[x] = shade(tex_tunnel[((d[q] + move) & 63) * TEXW + ((u + turn) & 63)], s[q]);
        }
        for (int x = split; x < EW; x++) {
            int q = x + ox - EW, u = top ? -a[q] : a[q];
            row_buf[x] = shade(tex_tunnel[((d[q] + move) & 63) * TEXW + ((u + turn) & 63)], s[q]);
        }
        put_row(fb, y, row_buf);
    }
}

static void part_roto(color_t *fb, float t)
{
    float ang = t * 0.7f, zoom = 0.9f + 0.55f * sinf(t * 0.9f) - 0.2f * beat;
    int32_t du = (int32_t)(cosf(ang) * zoom * 65536.0f), dv = (int32_t)(sinf(ang) * zoom * 65536.0f);
    int32_t u0 = (int32_t)(t * 20.0f * 65536.0f) - du * EW / 2 + dv * EH / 2;
    int32_t v0 = (int32_t)(t * 9.0f * 65536.0f) - dv * EW / 2 - du * EH / 2;
    for (int y = 0; y < EH; y++) {
        int32_t u = u0 - dv * y, w = v0 + du * y;
        for (int x = 0; x < EW; x++, u += du, w += dv)
            row_buf[x] = tex_roto[((w >> 16) & 63) * TEXW + ((u >> 16) & 63)];
        put_row(fb, y, row_buf);
    }
}

static void part_fire(color_t *fb, float t)
{
    for (int x = 0; x < EW; x++) {   /* two hot rows below the screen feed the flames */
        heat[EH][x] = (uint8_t)(sys_random() % 4 ? 255 : 60);
        heat[EH + 1][x] = (uint8_t)(sys_random() % 3 ? 255 : 0);
    }
    int ly = 70 + isin((int)(t * 60)) * 6 / 127;    /* the logo burns: its pixels are heat sources */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 128; x++)
            if (logo[y / 2][x / 2] && (sys_random() & 1)) heat[ly + y][16 + x] = 230;
    for (int y = 0; y < EH; y++) {
        for (int x = 0; x < EW; x++) {
            int l = x > 0 ? x - 1 : x, r = x < EW - 1 ? x + 1 : x;
            int sum = heat[y + 1][l] + heat[y + 1][x] + heat[y + 1][r] + heat[y + 2][x];
            int h = (sum * 63) >> 8;
            heat[y][x] = (uint8_t)h;
            row_buf[x] = pal_fire[h];
        }
        put_row(fb, y, row_buf);
    }
}

typedef struct {
    const char *name;
    void (*draw)(color_t *fb, float t);
} Part;

static const Part parts[NPARTS] = {
    { "starfield", part_stars },
    { "plasma", part_plasma },
    { "tunnel", part_tunnel },
    { "rotozoom", part_roto },
    { "fire", part_fire },
};

/* ------------------------------------------------------------------- panel */

static const char scroll_text[] =
    "      ***  PICOCALC STUDIO  ***   this whole demo was typed, compiled and linked on the "
    "calculator itself ...   six channel tracker synth at 16 kHz ...   plasma, tunnel, rotozoom "
    "and fire at 160x110 doubled ...   greetings to everyone still writing C on tiny machines!   ";

static void draw_panel(color_t *fb, float t, uint32_t ms)
{
    gfx_fill_rect(0, PANEL_Y, SCREEN_W, SCREEN_H - PANEL_Y, RGB(4, 4, 16));
    /* copper bars */
    for (int b = 0; b < 3; b++) {
        int cy = PANEL_Y + 40 + (int)(32.0f * sinf(t * 1.7f + (float)b * 2.1f));
        for (int i = -7; i <= 7; i++) {
            int k = 32 - abs(i) * 4;
            color_t c = b == 0 ? RGB(255, 60, 60) : b == 1 ? RGB(60, 255, 90) : RGB(80, 120, 255);
            gfx_hline(0, cy + i, SCREEN_W, shade(c, k));
        }
    }
    /* sine scroller: every screen column is a column of a 2x-scaled glyph with its own height */
    int len = (int)strlen(scroll_text);
    int off = (int)(t * 90.0f);
    for (int x = 0; x < SCREEN_W; x++) {
        int col = x + off;
        int ci = (col / 16) % len;
        int gx = (col % 16) / 2;
        int ch = scroll_text[ci];
        if (ch < 32 || ch > 126) continue;
        const uint8_t *g = font[ch - 32];
        int yb = PANEL_Y + 34 + isin(col * 2 + (int)(t * 160)) * 14 / 127;
        for (int gy = 0; gy < 8; gy++)
            if ((g[gy] >> (7 - gx)) & 1) {
                color_t c = hsv((float)col / 300.0f + (float)gy / 20.0f, 0.5f, 1.0f);
                color_t *p = fb + (yb + gy * 2) * SCREEN_W + x;
                p[0] = c;
                p[SCREEN_W] = shade(c, 20);
            }
    }
    /* footer: part, clock, channel meters */
    int part = (int)((ms / BAR_MS / 4 + (uint32_t)part_offset) % NPARTS);
    gfx_fill_rect(0, SCREEN_H - 22, SCREEN_W, 22, BLACK);
    gfx_printf(4, SCREEN_H - 18, LIGHTGRAY, "%-9s %u:%02u", parts[part].name, (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
    gfx_text(4, SCREEN_H - 9, muted ? "m unmute  n next  ESC" : "m mute  n next  ESC", GRAY);
    for (int c = 0; c < NCH; c++) {
        int x = 158 + c * 27, h = v[c].env * 14 / 32767;
        gfx_fill_rect(x, SCREEN_H - 20, 22, 14, RGB(20, 20, 40));
        gfx_fill_rect(x, SCREEN_H - 6 - h, 22, h, c == CH_KICK || c == CH_SNARE ? ORANGE : CYAN);
        gfx_text(x + 2, SCREEN_H - 19, ch_names[c], WHITE);
    }
}

/* ------------------------------------------------------------------- main */

static uint32_t t0, sent;

static void pump_audio(uint32_t ms)
{
    uint32_t target = (uint32_t)((uint64_t)ms * RATE / 1000) + LEAD_SAMPLES;
    if (target > sent + RATE / 2 || target < sent) sent = sample_pos = target - LEAD_SAMPLES;   /* stalled or jumped */
    while (sent < target) {
        int n = target - sent > BLOCK ? BLOCK : (int)(target - sent);
        synth(audio, n);
        snd_play(audio, n, RATE);
        sent += (uint32_t)n;
    }
}

void setup(void)
{
    capture_font();
    build_tables();
    snd_volume(6);
    gfx_clear(BLACK);
    t0 = sys_millis();
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) {
        snd_stop();
        free(tun_d); free(tun_a);
        sys_exit();
    }
    if (k == 'm') muted = !muted;
    if (k == 'n') part_offset++;

    uint32_t ms = sys_millis() - t0;
    pump_audio(ms);

    /* beat pulse from the song clock: 1 at every quarter note, fading over it */
    float qn = (float)ms / (float)(BAR_MS / 4);
    beat = 1.0f - (qn - floorf(qn));
    beat *= beat;

    uint32_t part_ms = ms % (BAR_MS * 4);
    int part = (int)((ms / BAR_MS / 4 + (uint32_t)part_offset) % NPARTS);
    float t = (float)ms / 1000.0f;
    color_t *fb = gfx_framebuffer();
    parts[part].draw(fb, t);

    /* fade in/out at the part edges */
    int fade = 32;
    if (part_ms < 400) fade = (int)(part_ms * 32 / 400);
    else if (part_ms > BAR_MS * 4 - 400) fade = (int)((BAR_MS * 4 - part_ms) * 32 / 400);
    if (fade < 32)
        for (int i = 0; i < SCREEN_W * PANEL_Y; i++) fb[i] = shade(fb[i], fade);

    draw_panel(fb, t, ms);
    gfx_flush();
    sys_yield();
}
