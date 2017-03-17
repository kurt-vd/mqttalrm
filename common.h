#include <string.h>
#include <time.h>
#include <stdarg.h>

#ifndef _common_h_
#define _common_h_

extern int strtohhmm(char *str);
extern int strtowdays(char *str);
extern time_t mktime_dstsafe(struct tm *tm);

__attribute__((format(printf,1,2)))
extern const char *csprintf(const char *fmt, ...);
#endif
