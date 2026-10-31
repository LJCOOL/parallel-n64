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
   SOFT_T_BAND0,     /* caller's band replay */
   SOFT_T_BAND1,     /* worker's band replay */
   SOFT_T_WAIT,      /* caller waiting for the worker */
   SOFT_T_COUNT
};

/* Reasons a span misses the tight fixed loop. */
enum
{
   SOFT_MISS_BLEND, SOFT_MISS_FOG, SOFT_MISS_ALPHA_TEST, SOFT_MISS_SPLIT_ALPHA, SOFT_MISS_CLAMP,
   SOFT_MISS_MIRROR, SOFT_MISS_NPOT, SOFT_MISS_UNTEXTURED, SOFT_MISS_GENERIC, SOFT_MISS_COUNT
};

/* Zeroes the triangle, pixel, path and program counters. */
void soft_stats_reset_counts(void);

#ifdef SOFT_STATS
#include <stdint.h>
/* Selects which thread's counter set this thread updates (0 or 1). */
void soft_stats_set_lane(int lane);
/* Pixel total over both lanes, for tests. */
uint64_t soft_stats_pixels(void);
void soft_stats_count_tri(void);
void soft_stats_count_px(int n);
void soft_stats_count_path(int generic, int n);
/* Tallies span pixels by the SOFT_MISS_* bits that kept them off the tight loop; zero means taken. */
void soft_stats_count_variant(unsigned miss, int n);
/* Tallies generic-path pixels per combine program; prog is a SoftCombineProgram. */
void soft_stats_count_program(const void *prog, int n);
uint64_t soft_stats_now(void);
void soft_stats_add(int stage, uint64_t start_ns);
#define SOFT_TIME_BEGIN(v)      uint64_t v = soft_stats_now()
#define SOFT_TIME_END(stage, v) soft_stats_add(stage, v)
#else
#define soft_stats_set_lane(l)  ((void)(l))
#define soft_stats_count_tri()  ((void)0)
#define soft_stats_count_px(n)  ((void)(n))
#define soft_stats_count_path(g, n) ((void)(n))
#define soft_stats_count_variant(m, n) ((void)(n))
#define soft_stats_count_program(p, n) ((void)(n))
#define SOFT_TIME_BEGIN(v)      ((void)0)
#define SOFT_TIME_END(stage, v) ((void)0)
#endif

#endif
