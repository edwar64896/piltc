#include <stdint.h>
#include <time.h>
#include "timef.h"

uint64_t getSecondsSinceMidnight(struct tm * tm) {
	uint64_t rc=
		(tm->tm_sec 		+
		 (tm->tm_min * 60)	+
		 (tm->tm_hour * 3600));
	return rc;
}
struct timespec * timespec_diff(struct timespec *start, struct timespec *stop, struct timespec *result)
{
				if ((stop->tv_nsec - start->tv_nsec) < 0) {
								result->tv_sec = stop->tv_sec - start->tv_sec - 1;
								result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
				} else {
								result->tv_sec = stop->tv_sec - start->tv_sec;
								result->tv_nsec = stop->tv_nsec - start->tv_nsec;
				}
				return result;
}

uint64_t timespec_to_uint64(struct timespec *input) {
				return (input->tv_sec*1e09)+input->tv_nsec;
}
