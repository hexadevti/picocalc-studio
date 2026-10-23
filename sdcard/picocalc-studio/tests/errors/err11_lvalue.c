// EXPECT: compile line=5 match=lvalue|assign|not assignable
int main(void)
{
    int a = 1;
    (a + 1) = 5;
    return a;
}
