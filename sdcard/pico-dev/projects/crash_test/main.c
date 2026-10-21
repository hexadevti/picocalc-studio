/* crash_test - deliberate faults to check the runtime's isolation and crash -> file:line mapping.
 * Each menu entry names the source line the IDE should jump to after the crash (see EXPECT tags).
 * Expected behaviour per entry:
 *   1 NULL write          -> crash report, editor opens at EXPECT:null
 *   2 infinite loop       -> Esc/Break stops it, IDE regains control, no reboot
 *   3 tight loop (no yield) -> Break still stops it (watchdog/forced task delete)
 *   4 stack overflow      -> stack guard trips, crash report at EXPECT:stack
 *   5 divide by zero      -> RISC-V does not trap: prints the defined result (-1 / dividend)
 *   6 illegal instruction -> crash report at EXPECT:illegal
 *   7 abort()             -> program ends with "abort" status
 *   8 bad free()          -> heap corruption detected, run aborted, IDE survives */
#include <picocalc.h>
#include <stdio.h>
#include <stdlib.h>

static volatile int sink;

static const char *const items[] = {
    "NULL pointer write", "infinite loop (yields)", "tight loop (no yield)", "stack overflow",
    "divide by zero", "illegal instruction", "abort()", "free() of a bad pointer",
};
#define NITEMS (int)(sizeof items / sizeof items[0])

static int sel;

static volatile int depth_limit = 0x7fffffff;   /* volatile: gcc can't prove the recursion is endless */

static int recurse(int n)
{
    volatile char pad[512];
    pad[0] = (char)n;
    if (n >= depth_limit) return 0;
    return recurse(n + 1) + pad[0];   /* EXPECT:stack */
}

static void run(int which)
{
    gfx_clear(BLACK);
    gfx_printf(10, 10, YELLOW, "running: %s", items[which]);
    gfx_text(10, 30, "Esc / Break should stop it", GRAY);
    gfx_flush();
    printf("crash_test %d\n", which + 1);
    switch (which) {
    case 0: {
        volatile int *p = NULL;
        *p = 42;   /* EXPECT:null */
        break;
    }
    case 1:
        for (;;) { sink++; sys_yield(); }
    case 2:
        for (;;) sink++;
    case 3:
        sink = recurse(0);
        break;
    case 4: {
        volatile int a = 7, b = 0;
        volatile unsigned ua = 7, ub = 0;
        int q = a / b, r = a % b;
        unsigned uq = ua / ub, ur = ua % ub;
        printf("7/0=%d 7%%0=%d 7u/0=%u 7u%%0=%u\n", q, r, uq, ur);
        gfx_printf(10, 60, WHITE, "7/0=%d 7%%0=%d", q, r);
        gfx_printf(10, 75, WHITE, "7u/0=%u 7u%%0=%u", uq, ur);
        gfx_text(10, 100, "(expect -1 7 4294967295 7)", GRAY);
        gfx_flush();
        kbd_wait();
        break;
    }
    case 5: {
        static const uint32_t bad[] = { 0x00000000u };   /* all-zero word is a defined illegal instruction */
        void (*fn)(void) = (void (*)(void))(uintptr_t)bad;
        fn();   /* EXPECT:illegal */
        break;
    }
    case 6:
        abort();
    case 7: {
        static int x;
        int *volatile p = &x;   /* laundered so gcc doesn't reject the bad free at compile time */
        free(p);
        break;
    }
    }
}

static void draw(void)
{
    gfx_clear(BLACK);
    gfx_text(10, 8, "crash_test - pick a fault", WHITE);
    for (int i = 0; i < NITEMS; i++) {
        int y = 30 + i * 16;
        if (i == sel) gfx_fill_rect(6, y - 2, 300, 14, DARKGRAY);
        gfx_printf(10, y, i == sel ? YELLOW : LIGHTGRAY, "%d  %s", i + 1, items[i]);
    }
    gfx_text(10, 300, "ENTER run   ESC quit", GRAY);
    gfx_flush();
}

void setup(void) { draw(); }

void loop(void)
{
    int k = kbd_wait();
    if (k == KEY_ESC) sys_exit();
    if (k == KEY_UP && sel > 0) sel--;
    if (k == KEY_DOWN && sel < NITEMS - 1) sel++;
    if (k >= '1' && k < '1' + NITEMS) { sel = k - '1'; k = KEY_ENTER; }
    if (k == KEY_ENTER) run(sel);
    draw();
}
