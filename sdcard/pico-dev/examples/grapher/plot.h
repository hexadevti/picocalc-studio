/* plot.h - world <-> screen mapping, grid with labelled ticks, curve drawing */
#ifndef PLOT_H
#define PLOT_H

#include <picocalc.h>
#include "expr.h"

typedef struct {
    float x0, x1, y0, y1;     /* visible world rectangle */
    int sx, sy, sw, sh;       /* screen rectangle */
} View;

int plot_sx(const View *v, float x);
int plot_sy(const View *v, float y);
float plot_wx(const View *v, int sx);          /* world x at the centre of screen column sx */
void plot_grid(const View *v);
/* style 0 solid, 1 dotted; returns the number of points drawn */
int plot_curve(const View *v, const Node *n, float t, color_t c, int style);
void plot_zoom(View *v, float factor);
void plot_pan(View *v, float fx, float fy);    /* by a fraction of the visible size */

#endif
