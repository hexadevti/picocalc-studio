// EXPECT: compile line=6 match=argument|too (many|few)
static int add(int a, int b) { return a + b; }

int main(void)
{
    return add(1);
}
