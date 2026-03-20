// hello.c : cross-compilation example (C + assembly)
#include <stdio.h>

// Defined in greeting.asm
extern const char *get_greeting(void);

int
main(void)
{
	printf("%s\n", get_greeting());
	return 0;
}
