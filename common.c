#include <stdlib.h>
#include <stdio.h>
#include "common.h"

/* utils */
int strtohhmm(char *str)
{
	char *next;
	int hh, mm;

	if (!str)
		return -1;
	hh = strtoul(str, &next, 10);
	if (next <= str || !strchr(":hHuU", *next))
		return -1;
	mm = strtoul(next+1, NULL, 10);
	return hh*100+mm;
}

int strtowdays(char *str)
{
	int j;
	int result = 0;

	for (j = 0; str[j] && (j < 7); ++j) {
		if (!strchr("-_", str[j]))
			/* enable this day,
			 * wday is struct tm.tm_wday compatible
			 * (sunday == 0)
			 */
			result |= 1 << ((j+1) % 7);
	}
	return result;
}

/* mktime, with DST crossing correction */
time_t mktime_dstsafe(struct tm *tm)
{
	struct tm copy = *tm;
	time_t result;

	result = mktime(tm);
	if ((tm->tm_min != copy.tm_min) || (tm->tm_hour != copy.tm_hour)) {
		/* crossed daylight saving settings */
		tm->tm_hour = copy.tm_hour;
		tm->tm_min = copy.tm_min;
		result = mktime(tm);
	}
	return result;
}

/* csprintf: like asprintf, but return a semi-static buffer,
 * so no need to free things
 */
static char *csprintf_ptr;

__attribute((destructor))
static void free_csprintf_ptr(void)
{
	if (csprintf_ptr)
		free(csprintf_ptr);
	csprintf_ptr = NULL;
}

__attribute__((format(printf,1,2)))
const char *csprintf(const char *fmt, ...)
{
	va_list va;

	free_csprintf_ptr();
	va_start(va, fmt);
	vasprintf(&csprintf_ptr, fmt, va);
	va_end(va);
	return csprintf_ptr;
}
