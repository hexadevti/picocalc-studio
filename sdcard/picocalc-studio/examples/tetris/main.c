/* tetris - heavier game logic: 2-D arrays, const 3-D piece tables, rotation with wall kicks,
 * line clears, gravity timing, sound effects, 7-bag randomiser. */
#include <picocalc.h>
#include <stdio.h>
#include <string.h>

#define W 10
#define H 20
#define CELL 15
#define OX 10
#define OY 10

static const uint16_t shapes[7][4] = {   /* 4x4 bitmasks for each rotation, row-major from bit 15 */
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },  /* I */
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 },  /* J */
    { 0x2E00, 0x4460, 0x0E80, 0xC440 },  /* L */
    { 0x6600, 0x6600, 0x6600, 0x6600 },  /* O */
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 },  /* S */
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 },  /* T */
    { 0xC600, 0x2640, 0x0C60, 0x4C80 },  /* Z */
};
static const color_t piece_color[7] = { CYAN, BLUE, ORANGE, YELLOW, GREEN, PURPLE, RED };

static uint8_t board[H][W];               /* 0 empty, else piece index + 1 */
static int cur, rot, px, py, next_piece;
static int bag[7], bag_pos = 7;
static int score, lines, level;
static uint32_t last_fall;
static bool over;

static bool cell(int piece, int r, int x, int y) { return (shapes[piece][r] >> (15 - (y * 4 + x))) & 1; }

static bool fits(int piece, int r, int nx, int ny)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cell(piece, r, x, y)) continue;
            int bx = nx + x, by = ny + y;
            if (bx < 0 || bx >= W || by >= H) return false;
            if (by >= 0 && board[by][bx]) return false;
        }
    return true;
}

static int draw_bag(void)
{
    if (bag_pos == 7) {
        for (int i = 0; i < 7; i++) bag[i] = i;
        for (int i = 6; i > 0; i--) { int j = (int)(sys_random() % (uint32_t)(i + 1)); int t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
        bag_pos = 0;
    }
    return bag[bag_pos++];
}

static void spawn(void)
{
    cur = next_piece;
    next_piece = draw_bag();
    rot = 0; px = 3; py = -1;
    if (!fits(cur, rot, px, py)) { over = true; snd_tone(98, 600); }
}

static void lock_piece(void)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(cur, rot, x, y) && py + y >= 0) board[py + y][px + x] = (uint8_t)(cur + 1);
    int cleared = 0;
    for (int y = H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < W; x++) full &= board[y][x] != 0;
        if (!full) continue;
        memmove(&board[1][0], &board[0][0], (size_t)y * W);
        memset(board[0], 0, W);
        cleared++;
        y++;   /* re-check the same row */
    }
    static const int pts[5] = { 0, 100, 300, 500, 800 };
    score += pts[cleared] * (level + 1);
    lines += cleared;
    level = lines / 10;
    if (cleared) snd_tone(660 + cleared * 220, 80);
    spawn();
}

static void rotate(int dir)
{
    int nr = (rot + dir + 4) & 3;
    static const int kicks[] = { 0, -1, 1, -2, 2 };
    for (unsigned i = 0; i < sizeof kicks / sizeof kicks[0]; i++)
        if (fits(cur, nr, px + kicks[i], py)) { rot = nr; px += kicks[i]; return; }
}

static void draw_block(int x, int y, color_t c)
{
    gfx_fill_rect(x + 1, y + 1, CELL - 2, CELL - 2, c);
    gfx_hline(x + 1, y + 1, CELL - 2, WHITE);
}

static void draw(void)
{
    gfx_clear(BLACK);
    gfx_rect(OX - 2, OY - 2, W * CELL + 4, H * CELL + 4, GRAY);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (board[y][x]) draw_block(OX + x * CELL, OY + y * CELL, piece_color[board[y][x] - 1]);
    int gy = py;   /* ghost piece */
    while (fits(cur, rot, px, gy + 1)) gy++;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(cur, rot, x, y)) {
                if (gy + y >= 0) gfx_rect(OX + (px + x) * CELL + 1, OY + (gy + y) * CELL + 1, CELL - 2, CELL - 2, DARKGRAY);
                if (py + y >= 0) draw_block(OX + (px + x) * CELL, OY + (py + y) * CELL, piece_color[cur]);
            }
    int sx = OX + W * CELL + 16;
    gfx_text(sx, 14, "NEXT", WHITE);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(next_piece, 0, x, y)) draw_block(sx + x * CELL, 30 + y * CELL, piece_color[next_piece]);
    gfx_printf(sx, 110, WHITE, "score %d", score);
    gfx_printf(sx, 126, WHITE, "lines %d", lines);
    gfx_printf(sx, 142, WHITE, "level %d", level);
    gfx_text(sx, 260, "arrows move", GRAY);
    gfx_text(sx, 272, "UP/z rotate", GRAY);
    gfx_text(sx, 284, "SPACE drop", GRAY);
    if (over) gfx_text(OX + 40, OY + 140, "GAME OVER", WHITE);
    gfx_flush();
}

void setup(void)
{
    sys_seed(sys_millis() ^ 0x5eed);
    next_piece = draw_bag();
    spawn();
    last_fall = sys_millis();
    draw();
}

void loop(void)
{
    int k = kbd_get();
    bool redraw = k != KEY_NONE;
    if (k == KEY_ESC) sys_exit();
    if (over) {
        if (k == KEY_ENTER) { memset(board, 0, sizeof board); score = lines = level = 0; over = false; spawn(); draw(); }
        sys_delay(10);
        return;
    }
    switch (k) {
    case KEY_LEFT: if (fits(cur, rot, px - 1, py)) px--; break;
    case KEY_RIGHT: if (fits(cur, rot, px + 1, py)) px++; break;
    case KEY_DOWN: if (fits(cur, rot, px, py + 1)) { py++; score++; } break;
    case KEY_UP: rotate(1); break;
    case 'z': rotate(-1); break;
    case ' ': while (fits(cur, rot, px, py + 1)) { py++; score += 2; } lock_piece(); break;
    }
    uint32_t now = sys_millis();
    uint32_t delay = (uint32_t)(level < 9 ? 800 - level * 80 : 80);
    if (now - last_fall >= delay) {
        last_fall = now;
        if (fits(cur, rot, px, py + 1)) py++;
        else lock_piece();
        redraw = true;
    }
    if (redraw) draw();
    sys_yield();
}
