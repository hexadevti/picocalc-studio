// EXPECT: compile line=2 match=no such file|not found|cannot open
#include "does_not_exist.h"
int main(void) { return 0; }
