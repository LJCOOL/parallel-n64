#ifndef SOFT_STATS_H
#define SOFT_STATS_H

#include "soft_backend.h"

#define SOFT_STATS_FRAMES 1800

#ifdef SOFT_STATS
void soft_stats_count_tri(void);
void soft_stats_count_px(int n);
#else
#define soft_stats_count_tri()  ((void)0)
#define soft_stats_count_px(n)  ((void)(n))
#endif

#endif
