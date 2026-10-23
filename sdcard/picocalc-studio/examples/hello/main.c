/* hello - the smallest SDK program, and the first export target. */
#include <picocalc.h>
#include <stdio.h>

static int frames;

void setup(void)
{
    printf("hello: setup\n");
    gfx_clear(NAVY);
    gfx_font(FONT_7X13);
    gfx_text(20, 20, "Hello, PicoCalc!", WHITE);
    gfx_font(FONT_6X10);
    gfx_text(20, 44, "Built and run on the device.", LIGHTGRAY);
    gfx_text(20, 300, "Press any key, ESC to quit", YELLOW);
    gfx_flush();
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) {
        printf("hello: %d frames\n", frames);
        sys_exit();
    }
    if (k != KEY_NONE) {
        gfx_fill_rect(20, 80, 280, 12, NAVY);
        gfx_printf(20, 80, GREEN, "key 0x%02X '%c'", k, (k >= 32 && k < 127) ? k : '?');
    }
    gfx_fill_rect(20, 100, 280, 12, NAVY);
    gfx_printf(20, 100, CYAN, "uptime %lu ms", (unsigned long)sys_millis());
    gfx_flush_rect(0, 80, SCREEN_W, 40);
    frames++;
    sys_delay(16);
}
