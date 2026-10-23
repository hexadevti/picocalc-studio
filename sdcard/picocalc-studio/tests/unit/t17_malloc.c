/* t17: heap - malloc/calloc/realloc/free stress with a deterministic PRNG and checksums.
 * Some blocks are deliberately leaked: the runtime must reclaim them when the program exits
 * (the device harness compares sys_free_heap() before and after the run). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rng = 12345;
static uint32_t next(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

typedef struct Node { int value; struct Node *next; } Node;

static Node *push(Node *head, int v) { Node *n = malloc(sizeof *n); n->value = v; n->next = head; return n; }
static Node *reverse(Node *h) { Node *p = NULL; while (h) { Node *n = h->next; h->next = p; p = h; h = n; } return p; }

int main(void)
{
    enum { SLOTS = 64, ROUNDS = 2000 };
    unsigned char *slot[SLOTS] = { 0 };
    size_t size[SLOTS] = { 0 };
    uint32_t check = 0;

    for (int r = 0; r < ROUNDS; r++) {
        int i = (int)(next() % SLOTS);
        if (slot[i] && next() % 3 == 0) {
            for (size_t k = 0; k < size[i]; k++) check += slot[i][k];
            free(slot[i]);
            slot[i] = NULL;
        } else if (slot[i]) {
            size_t ns = 1 + next() % 2048;
            unsigned char *p = realloc(slot[i], ns);
            if (!p) { printf("realloc failed\n"); return 1; }
            for (size_t k = size[i]; k < ns; k++) p[k] = (unsigned char)(k * 7 + (size_t)i);
            slot[i] = p; size[i] = ns;
        } else {
            size_t ns = 1 + next() % 1024;
            slot[i] = malloc(ns);
            if (!slot[i]) { printf("malloc failed\n"); return 1; }
            memset(slot[i], i, ns);
            size[i] = ns;
        }
    }
    for (int i = 0; i < SLOTS; i++)
        if (slot[i]) { for (size_t k = 0; k < size[i]; k++) check += slot[i][k]; free(slot[i]); }
    printf("stress check %u\n", check);

    int *z = calloc(1000, sizeof *z);
    int nz = 0;
    for (int i = 0; i < 1000; i++) nz += z[i] != 0;
    printf("calloc nonzero %d\n", nz);
    free(z);

    Node *list = NULL;
    for (int i = 1; i <= 10; i++) list = push(list, i * i);
    list = reverse(list);
    printf("list");
    for (Node *n = list; n; n = n->next) printf(" %d", n->value);
    printf("\n");
    while (list) { Node *n = list->next; free(list); list = n; }

    char *s = malloc(4);
    strcpy(s, "abc");
    s = realloc(s, 64);
    strcat(s, "-grown");
    printf("realloc %s\n", s);
    free(s);

    /* big allocation: lands in PSRAM on the device */
    size_t big = 4u * 1024u * 1024u;
    unsigned char *b = malloc(big);
    printf("big %s\n", b ? "ok" : "null");
    if (b) { b[0] = 1; b[big - 1] = 2; printf("big ends %d %d\n", b[0], b[big - 1]); free(b); }

    /* leak on purpose */
    for (int i = 0; i < 16; i++) malloc(100);
    printf("leaked 16 blocks\n");
    return 0;
}
