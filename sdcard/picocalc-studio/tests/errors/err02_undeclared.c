// EXPECT: compile line=5 match=undeclared|undefined|unknown
int main(void)
{
    int a = 1;
    return a + missing_var;
}
