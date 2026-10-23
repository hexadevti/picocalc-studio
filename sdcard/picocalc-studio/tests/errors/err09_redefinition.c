// EXPECT: compile line=4 match=redefin|redeclar|already|conflict
int value(void) { return 1; }

int value(void) { return 2; }

int main(void) { return value(); }
