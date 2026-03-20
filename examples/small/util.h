// util.h : shared utility interface
#ifndef UTIL_H
#define UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

// @doc Format a full name from first and last components.
// @param buf   destination buffer
// @param size  buffer size in bytes
// @param first first name
// @param last  last name
// @return pointer to buf
char *format_name(char *buf, int size, const char *first, const char *last);

#ifdef __cplusplus
}
#endif

#endif
