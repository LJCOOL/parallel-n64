#include <string.h>
#include <time.h>

#include "soft_stats.h"
#include "texcache.h"
#include "../../../libretro/libretro_private.h"

static uint64_t dl_start_ns;
static uint64_t dl_total_ns;
static unsigned frames;
static uint64_t tris, pixels;
static uint64_t stage_ns[SOFT_T_COUNT];
static unsigned busy_frames;
static int dl_this_frame;

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifdef SOFT_STATS
uint64_t soft_stats_now(void)
{
   return now_ns();
}

void soft_stats_add(int stage, uint64_t start_ns)
{
   stage_ns[stage] += now_ns() - start_ns;
}

void soft_stats_count_tri(void)
{
   tris++;
}

void soft_stats_count_px(int n)
{
   pixels += (uint64_t)n;
}
#endif


void soft_stats_begin(void)
{
   dl_start_ns = now_ns();
}

void soft_stats_end(void)
{
   dl_total_ns += now_ns() - dl_start_ns;
   dl_this_frame = 1;
}

void soft_stats_frame(void)
{
   busy_frames += dl_this_frame;
   dl_this_frame = 0;
   if (++frames < SOFT_STATS_FRAMES)
      return;

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: dl mean %.2f ms over %u frames, tex %u KB, tris %u/frame, px %u/frame\n",
            (double)dl_total_ns / 1e6 / frames, frames,
            (unsigned)(soft_texcache_bytes() / 1024),
            (unsigned)(tris / frames), (unsigned)(pixels / frames));
   if (log_cb && busy_frames)
   {
      double b = (double)busy_frames * 1e6;
      log_cb(RETRO_LOG_INFO, "soft: per busy frame (%u of %u): dl %.2f pipe %.2f texcrc %.2f decode %.2f raster %.2f present %.2f ms\n",
            busy_frames, frames, dl_total_ns / b, stage_ns[SOFT_T_PIPE] / b,
            stage_ns[SOFT_T_TEXCRC] / b, stage_ns[SOFT_T_DECODE] / b,
            stage_ns[SOFT_T_RASTER] / b, stage_ns[SOFT_T_PRESENT] / b);
   }
   memset(stage_ns, 0, sizeof(stage_ns));
   busy_frames = 0;
   dl_total_ns = 0;
   tris        = 0;
   pixels      = 0;
   frames      = 0;
}
