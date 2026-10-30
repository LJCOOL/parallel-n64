#include <time.h>

#include "soft_stats.h"
#include "../../../libretro/libretro_private.h"

static uint64_t dl_start_ns;
static uint64_t dl_total_ns;
static unsigned frames;

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
      log_cb(RETRO_LOG_INFO, "soft: dl mean %.2f ms over %u frames\n",
            (double)dl_total_ns / 1e6 / frames, frames);
   dl_total_ns = 0;
   frames      = 0;
}
