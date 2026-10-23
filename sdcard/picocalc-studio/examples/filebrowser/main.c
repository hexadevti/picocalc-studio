/* filebrowser - stdio + dirent on the SD card with a scrolling list UI.
 * Exercises opendir/readdir/stat, qsort with a module callback, dynamic arrays (realloc),
 * path string building and a hex/text file viewer.
 * UP/DOWN/PGUP/PGDN move, ENTER opens, BACKSPACE goes up, ESC quits. */
#include <picocalc.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROWS 27
#define ROW_H 11

typedef struct {
    char name[64];
    bool is_dir;
    long size;
} Entry;

static char cwd[256] = SD_ROOT;
static Entry *entries;
static int count, cap;
static int sel, top;

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void scan(void)
{
    count = sel = top = 0;
    DIR *d = opendir(cwd);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            Entry *n = realloc(entries, (size_t)cap * sizeof *entries);
            if (!n) break;
            entries = n;
        }
        Entry *e = &entries[count++];
        snprintf(e->name, sizeof e->name, "%.63s", de->d_name);
        char path[320];
        snprintf(path, sizeof path, "%s/%.63s", cwd, de->d_name);
        struct stat st;
        e->is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        e->size = e->is_dir ? 0 : (long)st.st_size;
    }
    closedir(d);
    qsort(entries, (size_t)count, sizeof *entries, cmp_entry);
}

static void draw(void)
{
    gfx_clear(BLACK);
    gfx_fill_rect(0, 0, SCREEN_W, 12, NAVY);
    gfx_printf(2, 1, WHITE, "%s  (%d)", cwd, count);
    for (int i = 0; i < ROWS && top + i < count; i++) {
        const Entry *e = &entries[top + i];
        int y = 14 + i * ROW_H;
        bool s = top + i == sel;
        if (s) gfx_fill_rect(0, y - 1, SCREEN_W, ROW_H, DARKGRAY);
        gfx_text(4, y, e->is_dir ? "[D]" : "   ", YELLOW);
        gfx_text(28, y, e->name, s ? WHITE : (e->is_dir ? CYAN : LIGHTGRAY));
        if (!e->is_dir) {
            char sz[16];
            if (e->size < 10240) snprintf(sz, sizeof sz, "%ld", e->size);
            else snprintf(sz, sizeof sz, "%ldK", e->size / 1024);
            gfx_text(SCREEN_W - 4 - gfx_text_width(sz), y, sz, GRAY);
        }
    }
    if (count > ROWS) {   /* scrollbar */
        int h = (SCREEN_H - 14) * ROWS / count;
        int y = 14 + (SCREEN_H - 14 - h) * top / (count - ROWS);
        gfx_fill_rect(SCREEN_W - 2, y, 2, h, GRAY);
    }
    gfx_flush();
}

static void view_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    long off = 0;
    bool hex = false;
    for (;;) {
        unsigned char buf[16 * 28];
        fseek(f, off, SEEK_SET);
        size_t n = fread(buf, 1, sizeof buf, f);
        gfx_clear(BLACK);
        gfx_fill_rect(0, 0, SCREEN_W, 12, NAVY);
        gfx_printf(2, 1, WHITE, "%s @%ld  H=hex", strrchr(path, '/') + 1, off);
        if (hex) {
            for (size_t r = 0; r * 8 < n && r < 28; r++) {
                char line[64];
                int p = snprintf(line, sizeof line, "%05lx ", (unsigned long)(off + (long)r * 8));
                for (size_t c = 0; c < 8 && r * 8 + c < n; c++) p += snprintf(line + p, sizeof line - (size_t)p, "%02x ", buf[r * 8 + c]);
                gfx_text(2, 14 + (int)r * ROW_H, line, LIGHTGRAY);
            }
        } else {
            int x = 2, y = 14;
            for (size_t i = 0; i < n && y < SCREEN_H - ROW_H; i++) {
                char ch[2] = { (char)(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.'), 0 };
                if (buf[i] == '\n' || x > SCREEN_W - 8) { x = 2; y += ROW_H; if (buf[i] == '\n') continue; }
                gfx_text(x, y, ch, LIGHTGRAY);
                x += gfx_font_w();
            }
        }
        gfx_flush();
        int k = kbd_wait();
        if (k == KEY_ESC || k == KEY_BACKSPACE) break;
        if (k == 'h') hex = !hex;
        if (k == KEY_DOWN || k == KEY_PGDN) { if (n == sizeof buf || (hex && n > 224)) off += hex ? 224 : (long)n / 2; }
        if ((k == KEY_UP || k == KEY_PGUP) && off > 0) { off -= hex ? 224 : (long)sizeof buf / 2; if (off < 0) off = 0; }
    }
    fclose(f);
}

void setup(void)
{
    scan();
    draw();
}

void loop(void)
{
    int k = kbd_wait();
    switch (k) {
    case KEY_ESC: free(entries); sys_exit();
    case KEY_UP: if (sel > 0) sel--; break;
    case KEY_DOWN: if (sel < count - 1) sel++; break;
    case KEY_PGUP: sel -= ROWS; if (sel < 0) sel = 0; break;
    case KEY_PGDN: sel += ROWS; if (sel > count - 1) sel = count - 1; if (sel < 0) sel = 0; break;
    case KEY_BACKSPACE: {
        char *slash = strrchr(cwd, '/');
        if (slash && slash != cwd && strcmp(cwd, SD_ROOT) != 0) { *slash = 0; scan(); }
        break;
    }
    case KEY_ENTER:
        if (count == 0) break;
        if (entries[sel].is_dir) {
            size_t len = strlen(cwd);
            snprintf(cwd + len, sizeof cwd - len, "/%s", entries[sel].name);
            scan();
        } else {
            char path[320];
            snprintf(path, sizeof path, "%s/%s", cwd, entries[sel].name);
            view_file(path);
        }
        break;
    }
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    draw();
}
