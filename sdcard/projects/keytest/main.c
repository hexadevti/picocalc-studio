/* keytest - shows every raw key event (code, state, modifiers) and a live map of held keys.
 * Validates kbd_event(), kbd_down(), kbd_mods(). ESC twice in a row quits. */
#include <picocalc.h>
#include <stdio.h>

#define LOG_LINES 18

static char log_buf[LOG_LINES][48];
static int log_count;
static int last_code;

static const char *key_name(int c)
{
    static char ch[4];
    switch (c) {
    case KEY_BACKSPACE: return "BKSP";
    case KEY_TAB: return "TAB";
    case KEY_ENTER: return "ENTER";
    case KEY_SPACE: return "SPACE";
    case KEY_ESC: return "ESC";
    case KEY_LEFT: return "LEFT";
    case KEY_RIGHT: return "RIGHT";
    case KEY_UP: return "UP";
    case KEY_DOWN: return "DOWN";
    case KEY_ALT: return "ALT";
    case KEY_SHIFT_L: return "LSHIFT";
    case KEY_SHIFT_R: return "RSHIFT";
    case KEY_SYM: return "SYM";
    case KEY_CTRL: return "CTRL";
    case KEY_CAPS: return "CAPS";
    case KEY_BREAK: return "BREAK";
    case KEY_INSERT: return "INS";
    case KEY_HOME: return "HOME";
    case KEY_DEL: return "DEL";
    case KEY_END: return "END";
    case KEY_PGUP: return "PGUP";
    case KEY_PGDN: return "PGDN";
    case KEY_F10: return "F10";
    }
    if (c >= KEY_F1 && c <= KEY_F9) { snprintf(ch, sizeof ch, "F%d", c - KEY_F1 + 1); return ch; }
    if (c > 32 && c < 127) { ch[0] = (char)c; ch[1] = 0; return ch; }
    return "?";
}

static void add_log(const key_event_t *e)
{
    static const char *states[] = { "?", "down", "held", "up" };
    if (log_count == LOG_LINES) {
        for (int i = 1; i < LOG_LINES; i++)
            snprintf(log_buf[i - 1], sizeof log_buf[0], "%s", log_buf[i]);
        log_count--;
    }
    snprintf(log_buf[log_count++], sizeof log_buf[0], "%02X %-6s %-4s %s%s%s%s", e->code, key_name(e->code),
             states[e->state & 3], e->mods & MOD_SHIFT ? "S" : "-", e->mods & MOD_CTRL ? "C" : "-",
             e->mods & MOD_ALT ? "A" : "-", e->mods & MOD_SYM ? "Y" : "-");
}

static void draw(void)
{
    static const int watch[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_SHIFT_L, KEY_SHIFT_R, KEY_CTRL, KEY_ALT,
                                 KEY_SYM, KEY_ENTER, KEY_SPACE, 'a', 'z' };
    gfx_clear(BLACK);
    gfx_text(4, 2, "keytest  (ESC ESC quits)", WHITE);
    gfx_hline(0, 14, SCREEN_W, GRAY);
    for (int i = 0; i < log_count; i++) gfx_text(4, 18 + i * 12, log_buf[i], i == log_count - 1 ? YELLOW : LIGHTGRAY);

    int y = 250;
    gfx_text(4, y, "held:", WHITE);
    int x = 40;
    for (unsigned i = 0; i < sizeof watch / sizeof watch[0]; i++) {
        bool down = kbd_down(watch[i]);
        const char *n = key_name(watch[i]);
        int w = gfx_text_width(n) + 6;
        if (x + w > SCREEN_W) { x = 40; y += 14; }
        gfx_fill_round_rect(x, y - 1, w, 12, 3, down ? GREEN : DARKGRAY);
        gfx_text(x + 3, y, n, down ? BLACK : GRAY);
        x += w + 3;
    }
    gfx_printf(4, 306, CYAN, "mods %02X", kbd_mods());
    gfx_flush();
}

void setup(void) { kbd_flush(); draw(); }

void loop(void)
{
    key_event_t e;
    bool changed = false;
    while (kbd_event(&e)) {
        add_log(&e);
        printf("key %02X state %d mods %02X\n", e.code, e.state, e.mods);
        if (e.state == KEY_PRESSED) {
            if (e.code == KEY_ESC && last_code == KEY_ESC) sys_exit();
            last_code = e.code;
        }
        changed = true;
    }
    if (changed) draw();
    sys_delay(10);
}
