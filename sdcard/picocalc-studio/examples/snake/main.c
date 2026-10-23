/* snake - classic game loop: fixed timestep with sys_millis(), arrays of structs, ring buffer,
 * random food, tone on eat, high score saved to the SD card. */
#include <picocalc.h>
#include <stdio.h>
#include <string.h>

#define CELL 10
#define COLS (SCREEN_W / CELL)
#define ROWS ((SCREEN_H - 20) / CELL)
#define MAXLEN (COLS * ROWS)
#define HISCORE_FILE SD_ROOT "/projects/snake/hiscore.txt"

typedef struct { int8_t x, y; } Cell;

static Cell body[MAXLEN];
static int head, len;              /* ring buffer: body[head] is the head */
static int dir_x = 1, dir_y;
static int next_dx = 1, next_dy;
static Cell food;
static int score, hiscore;
static bool dead;
static uint32_t last_step;
static int speed_ms = 120;
static uint8_t grid[COLS][ROWS];   /* occupancy for O(1) collision */

static void load_hiscore(void)
{
    FILE *f = fopen(HISCORE_FILE, "r");
    if (f) { if (fscanf(f, "%d", &hiscore) != 1) hiscore = 0; fclose(f); }
}

static void save_hiscore(void)
{
    FILE *f = fopen(HISCORE_FILE, "w");
    if (f) { fprintf(f, "%d\n", hiscore); fclose(f); }
}

static void place_food(void)
{
    do {
        food.x = (int8_t)(sys_random() % COLS);
        food.y = (int8_t)(sys_random() % ROWS);
    } while (grid[food.x][food.y]);
}

static void reset(void)
{
    memset(grid, 0, sizeof grid);
    len = 4;
    head = len - 1;
    for (int i = 0; i < len; i++) {
        body[i].x = (int8_t)(5 + i);
        body[i].y = ROWS / 2;
        grid[body[i].x][body[i].y] = 1;
    }
    dir_x = next_dx = 1;
    dir_y = next_dy = 0;
    score = 0;
    speed_ms = 120;
    dead = false;
    place_food();
}

static void draw(void)
{
    gfx_clear(BLACK);
    gfx_rect(0, 20, COLS * CELL, ROWS * CELL, DARKGRAY);
    for (int i = 0; i < len; i++) {
        Cell c = body[(head - i + MAXLEN) % MAXLEN];
        gfx_fill_rect(c.x * CELL + 1, 20 + c.y * CELL + 1, CELL - 2, CELL - 2, i == 0 ? YELLOW : GREEN);
    }
    gfx_fill_circle(food.x * CELL + CELL / 2, 20 + food.y * CELL + CELL / 2, CELL / 2 - 1, RED);
    gfx_printf(4, 4, WHITE, "score %d   best %d", score, hiscore);
    if (dead) {
        gfx_fill_round_rect(60, 130, 200, 50, 8, MAROON);
        gfx_text(100, 140, "GAME OVER", WHITE);
        gfx_text(76, 160, "ENTER again, ESC quit", LIGHTGRAY);
    }
    gfx_flush();
}

static void step(void)
{
    if ((next_dx != -dir_x) || (next_dy != -dir_y)) { dir_x = next_dx; dir_y = next_dy; }
    Cell h = body[head];
    h.x = (int8_t)(h.x + dir_x);
    h.y = (int8_t)(h.y + dir_y);
    if (h.x < 0 || h.x >= COLS || h.y < 0 || h.y >= ROWS || grid[h.x][h.y]) {
        dead = true;
        snd_tone(110, 400);
        if (score > hiscore) { hiscore = score; save_hiscore(); }
        return;
    }
    bool eat = h.x == food.x && h.y == food.y;
    if (!eat) {
        Cell tail = body[(head - len + 1 + MAXLEN) % MAXLEN];
        grid[tail.x][tail.y] = 0;
    } else {
        len++;
        score += 10;
        if (speed_ms > 50) speed_ms -= 4;
        snd_tone(880, 40);
    }
    head = (head + 1) % MAXLEN;
    body[head] = h;
    grid[h.x][h.y] = 1;
    if (eat) place_food();
}

void setup(void)
{
    sys_seed(sys_millis());
    load_hiscore();
    reset();
    last_step = sys_millis();
    draw();
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) sys_exit();
    if (dead) {
        if (k == KEY_ENTER) { reset(); draw(); }
        sys_delay(10);
        return;
    }
    switch (k) {
    case KEY_LEFT: next_dx = -1; next_dy = 0; break;
    case KEY_RIGHT: next_dx = 1; next_dy = 0; break;
    case KEY_UP: next_dx = 0; next_dy = -1; break;
    case KEY_DOWN: next_dx = 0; next_dy = 1; break;
    }
    uint32_t now = sys_millis();
    if (now - last_step >= (uint32_t)speed_ms) {
        last_step = now;
        step();
        draw();
    }
    sys_yield();
}
