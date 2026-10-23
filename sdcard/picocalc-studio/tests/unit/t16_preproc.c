/* t16: preprocessor - object/function macros, # and ##, conditionals, include of a local header,
 * __LINE__, variadic macros, macro recursion guard, #undef, defined(). */
#include <stdio.h>
#include "t16_helper.h"
#include "t16_helper.h"

#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define VERSION 3
#define LOG(fmt, ...) printf("[log] " fmt "\n", __VA_ARGS__)
#define LOG0(...) printf("[log0] " __VA_ARGS__)
#define EMPTY
#define PLUS +

#if VERSION >= 3 && defined(HELPER_MAGIC)
#define FEATURE "on"
#elif VERSION == 2
#define FEATURE "v2"
#else
#define FEATURE "off"
#endif

#ifdef NOT_DEFINED
#error "must be skipped"
#endif

#ifndef VERSION
#error "VERSION missing"
#endif

#define TEMP 1
#undef TEMP
#ifdef TEMP
#error "undef failed"
#endif

#if 0
this is not C and must be skipped entirely ' "
#endif

int main(void)
{
    int CAT(my, var) = 5;
    printf("square %d %d\n", SQUARE(7), SQUARE(1 + 2));
    printf("max %d\n", MAX(3, MAX(9, 4)));
    printf("str %s %s %s\n", STR(hello world), XSTR(VERSION), STR(a + b));
    printf("cat %d\n", myvar);
    printf("feature %s\n", FEATURE);
    printf("helper %d %d\n", HELPER_MAGIC, helper_twice(21));
    printf("line %d\n", __LINE__);
    LOG("%d-%s", 1, "x");
    LOG0("no args\n");
    int self = 1;
#define self self + 1          /* self-reference must expand only once, not recurse */
    printf("self %d\n", self);
#undef self
    printf("empty %d\n", 1 EMPTY PLUS 2);
    printf("multi %d\n",
#if VERSION > 1
           100
#else
           200
#endif
    );
    printf("builtin %d\n", __STDC__);
    return 0;
}
