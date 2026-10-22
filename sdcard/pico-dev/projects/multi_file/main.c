/* multi_file - main unit: ties shapes.c and score.c together. */
#include "game.h"
#include <stdio.h>

void setup(void)
{
    balls_init(MAX_BALLS);
    printf("%s: %d balls\n", g_title, g_ball_count);
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) sys_exit();
    if (k == ' ') score_add(10);
    balls_step();
    gfx_clear(BLACK);
    balls_draw();
    gfx_text(4, 4, g_title, WHITE);
    score_draw(4, SCREEN_H - 14);
    gfx_flush();
    sys_delay(20);
}
