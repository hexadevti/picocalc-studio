// editor.h - draws a Doc with line numbers and C highlighting, and applies editing keys to it.
#pragma once

#include "doc.h"

#define ED_X0 28           // text starts after a 4-digit line-number gutter
#define ED_COLS ((320 - ED_X0) / 6)

// Draws `rows` text lines at y0, scrolling first so the cursor is visible. errLine (0-based, or
// -1) gets a red background.
void ed_draw(Doc *d, int y0, int rows, int errLine);

// Applies a navigation or editing key (ui_key() code). False if the key isn't one. Alt (or Shift)+movement
// selects; Ctrl-A/C/X/V/Z/Y/K/D: select all, copy, cut, paste, undo, redo, delete line(s),
// duplicate line(s). Ctrl-Space toggles mark mode.
bool ed_key(Doc *d, int key, int rows);

// Mark mode on (selection starts at the cursor, movement extends it) or off (selection kept).
void ed_mark(Doc *d);
void ed_unselect(Doc *d);
bool ed_has_sel(const Doc *d);
// Replaces the selection (or inserts at the cursor). The caller calls doc_begin() first.
void ed_replace_sel(Doc *d, const char *s, int rows);

// Moves the cursor to (line, col), 0-based and clamped, and centres the line on screen.
void ed_goto(Doc *d, int line, int col, int rows);

// Finds `s` (ignoring case) after the cursor, wrapping around, and
// selects it. False if it isn't in the doc.
bool ed_find(Doc *d, const char *s, int rows);
// The same, from (y, x) on: a match right there counts.
bool ed_find_at(Doc *d, const char *s, int rows, int y, int x);
