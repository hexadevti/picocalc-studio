/* textedit_mini - a tiny line editor: array of heap-allocated lines, insert/delete with
 * realloc + memmove, cursor movement, file load and save.
 * Arrows move, typing inserts, ENTER splits, BACKSPACE/DEL join, F2 save, ESC quits. */
#include <picocalc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_PATH SD_ROOT "/projects/textedit_mini/notes.txt"
#define VIS_ROWS 28
#define VIS_COLS 52

typedef struct {
    char *s;
    int len;
} Line;

static Line *lines;
static int nlines, cap;
static int row, col, top, left;
static bool dirty;
static char msg[48];

static void line_init(Line *l, const char *s, int len)
{
    l->s = malloc((size_t)len + 1);
    memcpy(l->s, s, (size_t)len);
    l->s[len] = 0;
    l->len = len;
}

static void insert_line(int at, const char *s, int len)
{
    if (nlines == cap) {
        cap = cap ? cap * 2 : 32;
        lines = realloc(lines, (size_t)cap * sizeof *lines);
    }
    memmove(&lines[at + 1], &lines[at], (size_t)(nlines - at) * sizeof *lines);
    line_init(&lines[at], s, len);
    nlines++;
}

static void delete_line(int at)
{
    free(lines[at].s);
    memmove(&lines[at], &lines[at + 1], (size_t)(nlines - at - 1) * sizeof *lines);
    nlines--;
}

static void load(void)
{
    FILE *f = fopen(FILE_PATH, "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof buf, f)) {
            int n = (int)strcspn(buf, "\r\n");
            insert_line(nlines, buf, n);
        }
        fclose(f);
        snprintf(msg, sizeof msg, "loaded %d lines", nlines);
    }
    if (nlines == 0) insert_line(0, "", 0);
}

static void save(void)
{
    FILE *f = fopen(FILE_PATH, "w");
    if (!f) { snprintf(msg, sizeof msg, "save failed"); return; }
    long bytes = 0;
    for (int i = 0; i < nlines; i++) bytes += fprintf(f, "%s\n", lines[i].s);
    fclose(f);
    dirty = false;
    snprintf(msg, sizeof msg, "saved %ld bytes", bytes);
}

static void insert_char(char c)
{
    Line *l = &lines[row];
    l->s = realloc(l->s, (size_t)l->len + 2);
    memmove(l->s + col + 1, l->s + col, (size_t)(l->len - col + 1));
    l->s[col++] = c;
    l->len++;
    dirty = true;
}

static void split_line(void)
{
    Line *l = &lines[row];
    insert_line(row + 1, l->s + col, l->len - col);
    l = &lines[row];   /* lines may have moved */
    l->s[col] = 0;
    l->len = col;
    row++;
    col = 0;
    dirty = true;
}

static void join_with_next(int r)
{
    Line *a = &lines[r], *b = &lines[r + 1];
    a->s = realloc(a->s, (size_t)(a->len + b->len + 1));
    memcpy(a->s + a->len, b->s, (size_t)b->len + 1);
    a->len += b->len;
    delete_line(r + 1);
    dirty = true;
}

static void draw(void)
{
    gfx_clear(BLACK);
    int fh = gfx_font_h();
    for (int i = 0; i < VIS_ROWS && top + i < nlines; i++) {
        const Line *l = &lines[top + i];
        char vis[VIS_COLS + 1];
        int n = l->len - left;
        if (n < 0) n = 0;
        if (n > VIS_COLS) n = VIS_COLS;
        memcpy(vis, l->s + (l->len > left ? left : l->len), (size_t)n);
        vis[n] = 0;
        gfx_text(2, 2 + i * fh, vis, WHITE);
    }
    int cx = 2 + (col - left) * gfx_font_w(), cy = 2 + (row - top) * fh;
    gfx_fill_rect(cx, cy, 2, fh, YELLOW);
    gfx_fill_rect(0, SCREEN_H - 12, SCREEN_W, 12, DARKGRAY);
    gfx_printf(2, SCREEN_H - 11, WHITE, "%d:%d %s %s", row + 1, col + 1, dirty ? "*" : " ", msg);
    gfx_flush();
}

void setup(void)
{
    load();
    draw();
}

void loop(void)
{
    int k = kbd_wait();
    msg[0] = 0;
    switch (k) {
    case KEY_ESC:
        if (dirty) save();
        for (int i = 0; i < nlines; i++) free(lines[i].s);
        free(lines);
        sys_exit();
    case KEY_F2: save(); break;
    case KEY_LEFT: if (col > 0) col--; else if (row > 0) { row--; col = lines[row].len; } break;
    case KEY_RIGHT: if (col < lines[row].len) col++; else if (row < nlines - 1) { row++; col = 0; } break;
    case KEY_UP: if (row > 0) row--; break;
    case KEY_DOWN: if (row < nlines - 1) row++; break;
    case KEY_HOME: col = 0; break;
    case KEY_END: col = lines[row].len; break;
    case KEY_ENTER: split_line(); break;
    case KEY_BACKSPACE:
        if (col > 0) {
            Line *l = &lines[row];
            memmove(l->s + col - 1, l->s + col, (size_t)(l->len - col + 1));
            l->len--;
            col--;
            dirty = true;
        } else if (row > 0) {
            col = lines[row - 1].len;
            join_with_next(--row);
        }
        break;
    case KEY_DEL:
        if (col < lines[row].len) {
            Line *l = &lines[row];
            memmove(l->s + col, l->s + col + 1, (size_t)(l->len - col));
            l->len--;
            dirty = true;
        } else if (row < nlines - 1) join_with_next(row);
        break;
    default:
        if (k == KEY_TAB) { for (int i = 0; i < 4; i++) insert_char(' '); }
        else if (k >= 32 && k < 127) insert_char((char)k);
    }
    if (col > lines[row].len) col = lines[row].len;
    if (row < top) top = row;
    if (row >= top + VIS_ROWS) top = row - VIS_ROWS + 1;
    if (col < left) left = col;
    if (col >= left + VIS_COLS) left = col - VIS_COLS + 1;
    draw();
}
