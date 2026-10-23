// EXPECT: link line=0 match=main|setup|entry
/* a program with neither main() nor setup()/loop() cannot be run */
static int helper(int x) { return x * 2; }
int compute(void) { return helper(21); }
