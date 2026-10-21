/* t09: char signedness (plain char is UNSIGNED on RISC-V), literals, escapes, <string.h>, <ctype.h>. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static size_t my_strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }

static void reverse(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
}

int main(void)
{
    char c = (char)200;
    signed char sc = (signed char)200;
    printf("signedness %d %d %d\n", c, sc, c > 0);

    printf("escapes [%s] %d %d %d %d\n", "a\tb", '\n', '\0', '\x41', '\101');
    printf("quote \"q\" \\ %c\n", '\'');
    printf("concat %s\n", "con" "cat" "enated");
    printf("sizeof %d %d\n", (int)sizeof("abc"), (int)sizeof('a'));

    char buf[64];
    strcpy(buf, "Hello");
    strcat(buf, ", PicoCalc");
    printf("cpy %s len %d %d\n", buf, (int)strlen(buf), (int)my_strlen(buf));
    printf("cmp %d %d %d\n", strcmp("abc", "abd") < 0, strcmp("b", "a") > 0, strcmp("same", "same"));
    printf("ncmp %d\n", strncmp("prefix-one", "prefix-two", 7));
    printf("chr %s\n", strchr(buf, ','));
    printf("rchr %s\n", strrchr("a/b/c.txt", '/'));
    printf("str %s\n", strstr(buf, "Pico"));

    char tmp[16];
    memset(tmp, '-', sizeof tmp);
    tmp[15] = 0;
    memcpy(tmp, "XYZ", 3);
    memmove(tmp + 2, tmp, 5);
    printf("mem %s %d\n", tmp, memcmp("aaa", "aab", 3) < 0);

    strcpy(buf, "stressed");
    reverse(buf);
    printf("rev %s\n", buf);

    int upper = 0, digits = 0, spaces = 0;
    const char *text = "Hello World 2026!";
    for (const char *p = text; *p; p++) {
        if (isupper((unsigned char)*p)) upper++;
        if (isdigit((unsigned char)*p)) digits++;
        if (isspace((unsigned char)*p)) spaces++;
    }
    printf("ctype %d %d %d %c %c\n", upper, digits, spaces, toupper('q'), tolower('Q'));

    printf("atoi %d %ld %lu\n", atoi("-1234"), strtol("ff", NULL, 16), strtoul("777", NULL, 8));

    char out[32];
    int n = snprintf(out, sizeof out, "%s-%03d", "id", 7);
    printf("snprintf %s %d\n", out, n);
    n = snprintf(out, 5, "truncated");
    printf("trunc %s %d\n", out, n);

    char arr[] = "array";
    printf("arrsize %d\n", (int)sizeof arr);
    return 0;
}
