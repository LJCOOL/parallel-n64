#ifndef SOFT_STATS_H
#define SOFT_STATS_H

#include "soft_backend.h"

#define SOFT_STATS_FRAMES 1800

/* Timed renderer stages; stages may nest, so totals can overlap. */
enum
{
   SOFT_T_PIPE,      /* per-draw state build, includes texture lookup */
   SOFT_T_TEXCRC,    /* TMEM hashing for the cache key */
   SOFT_T_DECODE,    /* texture decode on a cache miss */
   SOFT_T_RASTER,    /* triangle setup and spans */
   SOFT_T_PRESENT,   /* RGB565 to XRGB8888 widening */
   SOFT_T_COUNT
};

#ifdef SOFT_STATS
#include <stdint.h>
void soft_stats_count_tri(void);
void soft_stats_count_px(int n);
void soft_stats_count_path(int generic, int n);
uint64_t soft_stats_now(void);
void soft_stats_add(int stage, uint64_t start_ns);
#define SOFT_TIME_BEGIN(v)      uint64_t v = soft_stats_now()
#define SOFT_TIME_END(stage, v) soft_stats_add(stage, v)
#else
#define soft_stats_count_tri()  ((void)0)
#define soft_stats_count_px(n)  ((void)(n))
#define soft_stats_count_path(g, n) ((void)(n))
#define SOFT_TIME_BEGIN(v)      ((void)0)
#define SOFT_TIME_END(stage, v) ((void)0)
#endif

#endif
