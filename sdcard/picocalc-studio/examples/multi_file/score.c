/* multi_file - score keeping; has its own static clamp() to test static symbol isolation. */
#include "game.h"

const char *const g_title = "multi_file demo";
static int score;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void score_add(int pts) { score = clamp(score + pts, 0, 999999); }
int score_get(void) { return score; }

void score_draw(int x, int y)
{
    gfx_printf(x, y, YELLOW, "score %06d", score_get());
}
