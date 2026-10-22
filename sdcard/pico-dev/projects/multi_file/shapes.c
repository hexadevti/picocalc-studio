/* multi_file - ball physics; static helpers must stay private to this unit. */
#include "game.h"

Ball g_balls[MAX_BALLS];
int g_ball_count;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }   /* same name as in score.c */

static const color_t palette[] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, ORANGE, WHITE };

void balls_init(int n)
{
    sys_seed(1234);
    g_ball_count = clamp(n, 0, MAX_BALLS);
    for (int i = 0; i < g_ball_count; i++) {
        Ball *b = &g_balls[i];
        b->r = 4 + (int)(sys_random() % 10);
        b->x = b->r + (int)(sys_random() % (SCREEN_W - 2 * b->r));
        b->y = b->r + (int)(sys_random() % (SCREEN_H - 2 * b->r));
        b->dx = (int)(sys_random() % 7) - 3;
        b->dy = (int)(sys_random() % 7) - 3;
        if (!b->dx) b->dx = 1;
        b->c = palette[i % 8];
    }
}

void balls_step(void)
{
    for (int i = 0; i < g_ball_count; i++) {
        Ball *b = &g_balls[i];
        b->x += b->dx;
        b->y += b->dy;
        if (b->x < b->r || b->x > SCREEN_W - b->r) { b->dx = -b->dx; score_add(1); }
        if (b->y < b->r || b->y > SCREEN_H - b->r) { b->dy = -b->dy; score_add(1); }
        b->x = clamp(b->x, b->r, SCREEN_W - b->r);
        b->y = clamp(b->y, b->r, SCREEN_H - b->r);
    }
}

void balls_draw(void)
{
    for (int i = 0; i < g_ball_count; i++)
        gfx_fill_circle(g_balls[i].x, g_balls[i].y, g_balls[i].r, g_balls[i].c);
}
