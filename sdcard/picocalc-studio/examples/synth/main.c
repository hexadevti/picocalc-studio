/* synth - audio streaming: the keyboard rows are a piano, samples are generated in the module and
 * queued with snd_play() (firmware blocks while its queue is full). Exercises int16 buffers, a
 * phase accumulator, ADSR envelope, waveform selection and volume control.
 *   a w s e d f t g y h u j k  -> notes C4..C5     1-4 waveform   [ ] volume   ESC quit */
#include <picocalc.h>
#include <stdio.h>
#include <string.h>

#define RATE 22050
#define BLOCK 256

enum Wave { SQUARE, SAW, TRIANGLE, NOISE };
static const char *const wave_names[] = { "square", "saw", "triangle", "noise" };

static const char keys[] = "awsedftgyhujk";
static const uint16_t freq_x10[] = { 2616, 2772, 2937, 3111, 3296, 3492, 3700, 3920, 4153, 4400, 4662, 4939, 5233 };

static enum Wave wave = SQUARE;
static int volume = 6;
static uint32_t phase, phase_inc;
static int env;                 /* 0..32767 */
static int env_target;
static int note = -1;
static uint32_t note_until;
static uint32_t noise = 0x12345;
static int16_t buf[BLOCK];

static int16_t osc(void)
{
    phase += phase_inc;
    switch (wave) {
    case SQUARE: return (int16_t)((phase & 0x80000000u) ? 12000 : -12000);
    case SAW: return (int16_t)((int32_t)(phase >> 16) - 32768);
    case TRIANGLE: {
        int32_t p = (int32_t)(phase >> 16);
        return (int16_t)((p < 32768 ? p * 2 : (65535 - p) * 2) - 32768);
    }
    default:
        noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
        return (int16_t)(noise & 0xFFFF);
    }
}

static void fill_block(void)
{
    for (int i = 0; i < BLOCK; i++) {
        if (env < env_target) env += 400; else if (env > env_target) env -= 60;   /* fast attack, slow release */
        if (env < 0) env = 0;
        if (env > 32767) env = 32767;
        buf[i] = (int16_t)(((int32_t)osc() * env) >> 15);
    }
}

static void draw(void)
{
    gfx_clear(BLACK);
    gfx_font(FONT_7X13);
    gfx_text(8, 6, "synth", WHITE);
    gfx_font(FONT_6X10);
    gfx_printf(8, 26, LIGHTGRAY, "wave %s  volume %d", wave_names[wave], volume);
    for (int i = 0; i < 13; i++) {
        int x = 8 + i * 23;
        bool sharp = strchr("wetyu", keys[i]) != NULL;
        gfx_fill_rect(x, sharp ? 60 : 90, 20, sharp ? 30 : 60, i == note ? YELLOW : (sharp ? DARKGRAY : WHITE));
        char label[2] = { keys[i], 0 };
        gfx_text(x + 7, 160, label, CYAN);
    }
    /* waveform preview of the last block */
    for (int i = 0; i < BLOCK - 1 && i < SCREEN_W - 16; i++)
        gfx_line(8 + i, 240 - buf[i] / 512, 9 + i, 240 - buf[i + 1] / 512, GREEN);
    gfx_flush();
}

void setup(void)
{
    snd_volume(volume);
    draw();
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) { snd_stop(); sys_exit(); }
    if (k >= '1' && k <= '4') { wave = (enum Wave)(k - '1'); draw(); }
    if (k == '[' && volume > 0) { snd_volume(--volume); draw(); }
    if (k == ']' && volume < 10) { snd_volume(++volume); draw(); }
    const char *p = k > 0 && k < 128 ? strchr(keys, k) : NULL;
    if (p && *p) {
        note = (int)(p - keys);
        phase_inc = (uint32_t)((uint64_t)freq_x10[note] * 429496730ull / RATE);   /* (f*10) * (2^32/10) / rate */
        env_target = 32767;
        note_until = sys_millis() + 250;
        draw();
    }
    if (note >= 0 && sys_millis() > note_until) { env_target = 0; note = -1; draw(); }
    fill_block();
    snd_play(buf, BLOCK, RATE);    /* paces the loop: blocks while the queue is full */
}
