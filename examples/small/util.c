// util.c : utility functions
#include "util.h"

#include <stdio.h>

// @doc Formats "first last" into buf, truncating if necessary.
char *
format_name(char *buf, int size, const char *first, const char *last)
{
	snprintf(buf, size, "%s %s", first, last);
	return buf;
}
