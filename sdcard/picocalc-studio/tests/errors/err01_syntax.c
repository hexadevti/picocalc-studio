// EXPECT: compile line=5 match=expect|syntax|unexpected
int main(void)
{
    int x = 1;
    int y = (x + 2;
    return y;
}
