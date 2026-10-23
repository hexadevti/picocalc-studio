// EXPECT: compile line=6 match=incompatible|type|struct|invalid
struct P { int x, y; };
int main(void)
{
    struct P p = { 1, 2 };
    int n = p;
    return n;
}
