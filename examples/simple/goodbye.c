// goodbye.c : say farewell
#include "greet.h"

#include <stdio.h>

int
main(void)
{
	greet("world");
	printf("Goodbye!\n");
	return 0;
}
