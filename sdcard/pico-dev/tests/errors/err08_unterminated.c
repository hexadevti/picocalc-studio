// EXPECT: compile line=4 match=unterminated|missing|string|quote
int main(void)
{
    const char *s = "never closed;
    return 0;
}
