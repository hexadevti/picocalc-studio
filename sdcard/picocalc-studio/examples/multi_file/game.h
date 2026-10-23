/* multi_file - shared declarations across three translation units. */
#ifndef GAME_H
#define GAME_H
#include <picocalc.h>

typedef struct { int x, y, r; color_t c; int dx, dy; } Ball;

#define MAX_BALLS 12

extern Ball g_balls[MAX_BALLS];   /* defined in shapes.c */
extern int g_ball_count;
extern const char *const g_title; /* defined in score.c */

void balls_init(int n);
void balls_step(void);
void balls_draw(void);

void score_add(int pts);
int  score_get(void);
void score_draw(int x, int y);

#endif
