#include <time.h>

#ifndef _sun_h_
#define _sun_h_

#ifdef __cplusplus
extern "C" {
#endif

extern int sungetpos(time_t now, double north, double east,
		double *pincl, double *pazimuth,
		unsigned int *secs_to_sunupdown);

/* legacy function */
static inline int where_is_the_sun(time_t now, double north, double east,
		double *pincl, double *pazimuth)
{
	return sungetpos(now, north, east, pincl, pazimuth, NULL);
}

#ifdef __cplusplus
}
#endif
#endif
