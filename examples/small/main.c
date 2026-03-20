// main.c : program entry point (C)
#include "config.h"
#include "util.h"

#include <stdio.h>

// defined in printer.cpp
#ifdef __cplusplus
extern "C"
#endif
void fancy_print(const char *text);

int
main(void)
{
	char buf[128];
	printf("%s v%s\n", APP_NAME, APP_VERSION);
	format_name(buf, sizeof(buf), "Jane", "Doe");
	fancy_print(buf);
	return 0;
}
