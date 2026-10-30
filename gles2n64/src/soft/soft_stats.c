#include <time.h>

#include "soft_stats.h"
#include "texcache.h"
#include "../../../libretro/libretro_private.h"

static uint64_t dl_start_ns;
static uint64_t dl_total_ns;
static unsigned frames;
static uint64_t tris, pixels;

#ifdef SOFT_STATS
void soft_stats_count_tri(void)
{
   tris++;
}

void soft_stats_count_px(int n)
{
   pixels += (uint64_t)n;
}
#endif

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void soft_stats_begin(void)
{
   dl_start_ns = now_ns();
}

void soft_stats_end(void)
{
   dl_total_ns += now_ns() - dl_start_ns;
}

void soft_stats_frame(void)
{
   if (++frames < SOFT_STATS_FRAMES)
      return;

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: dl mean %.2f ms over %u frames, tex %u KB, tris %u/frame, px %u/frame\n",
            (double)dl_total_ns / 1e6 / frames, frames,
            (unsigned)(soft_texcache_bytes() / 1024),
            (unsigned)(tris / frames), (unsigned)(pixels / frames));
   dl_total_ns = 0;
   tris        = 0;
   pixels      = 0;
   frames      = 0;
}
