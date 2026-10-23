/* t08: unions (type punning, size), enums (implicit/explicit values), typedefs, anonymous members. */
#include <stdio.h>

typedef unsigned int u32;
typedef unsigned char byte;

typedef union {
    u32 word;
    byte b[4];
    float f;
} Pun;

enum Color { RED, GREEN, BLUE = 10, CYAN, MAGENTA = -3, YELLOW };
typedef enum { OFF, ON } Switch;

typedef struct {
    int kind;
    union {               /* anonymous union */
        int i;
        float f;
        const char *s;
    };
} Value;

typedef int (*binop)(int, int);
static int add(int a, int b) { return a + b; }

static void show(const Value *v)
{
    switch (v->kind) {
    case 0: printf("int %d\n", v->i); break;
    case 1: printf("float %.2f\n", v->f); break;
    default: printf("str %s\n", v->s); break;
    }
}

int main(void)
{
    Pun p;
    p.word = 0x41424344u;
    printf("pun %c%c%c%c size %d\n", p.b[0], p.b[1], p.b[2], p.b[3], (int)sizeof p);
    p.f = 1.0f;
    printf("float bits %08x\n", p.word);

    printf("enum %d %d %d %d %d %d\n", RED, GREEN, BLUE, CYAN, MAGENTA, YELLOW);
    enum Color c = CYAN;
    c++;
    printf("enum inc %d size %d\n", (int)c, (int)sizeof(enum Color));
    Switch s = ON;
    printf("switch %d\n", s == ON);

    Value vals[3];
    vals[0].kind = 0; vals[0].i = 42;
    vals[1].kind = 1; vals[1].f = 3.25f;
    vals[2].kind = 2; vals[2].s = "text";
    for (int i = 0; i < 3; i++) show(&vals[i]);

    binop op = add;
    u32 total = (u32)op(40, 2);
    printf("typedef fp %u\n", total);

    union { long long ll; int half[2]; } u;
    u.ll = 0x0000000500000007LL;
    printf("halves %d %d\n", u.half[0], u.half[1]);
    return 0;
}
