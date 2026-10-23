// EXPECT: compile line=4 match=stop here
#define LEVEL 2
#if LEVEL > 1
#error "stop here"
#endif
int main(void) { return 0; }
