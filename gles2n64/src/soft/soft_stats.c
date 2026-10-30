#include <string.h>
#include <time.h>

#include "soft_stats.h"
#include "texcache.h"
#include "raster.h"
#include "spans.h"
#include "combine.h"
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

static uint64_t span_px[2];

#ifdef SOFT_STATS
void soft_stats_count_path(int generic, int n)
{
   span_px[generic != 0] += (uint64_t)n;
}

/* Times synthetic full-screen draws, small triangles and the clock itself on this CPU. */
static void calibrate(void)
{
   static uint16_t col[SOFT_WIDTH * SOFT_HEIGHT], dep[SOFT_WIDTH * SOFT_HEIGHT];
   static uint16_t trgb[32 * 32];
   static uint8_t ta[32 * 32];
   SoftTex tex;
   SoftCombineProgram prog;
   SoftTarget t;
   SoftPipe p;
   SoftVert v[4];
   uint64_t t0, ns[3];
   int i, k, pass;

   for (i = 0; i < 32 * 32; i++) { trgb[i] = (uint16_t)(i * 2654435761u >> 16); ta[i] = 255; }
   memset(&tex, 0, sizeof(tex));
   tex.rgb = trgb; tex.a = ta; tex.w = 32; tex.h = 32; tex.mask_s = 31; tex.mask_t = 31;
   memset(&prog, 0, sizeof(prog));
   prog.cycles = 1; prog.uses_tex = 1; prog.uses_shade = 1;
   prog.sel[0][0][0] = TEXEL0; prog.sel[0][0][1] = ZERO; prog.sel[0][0][2] = SHADE; prog.sel[0][0][3] = ZERO;
   prog.sel[0][1][0] = TEXEL0; prog.sel[0][1][1] = ZERO; prog.sel[0][1][2] = SHADE; prog.sel[0][1][3] = ZERO;
   soft_target_init(&t, col, dep, SOFT_WIDTH, SOFT_HEIGHT);
   memset(&p, 0, sizeof(p));
   p.span = SOFT_SPAN_COMBINE; p.cull = SOFT_CULL_NONE; p.tex = &tex; p.prog = &prog;
   p.z_test = 1; p.z_update = 1;
   memset(v, 0, sizeof(v));
   for (i = 0; i < 4; i++)
   {
      v[i].x = (i & 1) ? SOFT_WIDTH * 16 : 0; v[i].y = (i & 2) ? SOFT_HEIGHT * 16 : 0;
      v[i].z = 100; v[i].r = v[i].g = v[i].b = v[i].a = 200; v[i].invw = 1.0f;
      v[i].s = (i & 1) ? 64.0f : 0.0f; v[i].t = (i & 2) ? 64.0f : 0.0f;
   }
   /* Full screen four times per mode: fast path, then generic. */
   for (pass = 0; pass < 2; pass++)
   {
      prog.mode = pass ? SC_GENERIC : SC_TEX_MUL_SHADE;
      t0 = now_ns();
      for (k = 0; k < 4; k++)
      {
         memset(dep, 0xff, sizeof(dep));
         soft_draw_tri(&t, &v[0], &v[1], &v[3], &p);
         soft_draw_tri(&t, &v[0], &v[3], &v[2], &p);
      }
      ns[pass] = now_ns() - t0;
   }
   /* 2000 eight-pixel triangles measure per-triangle overhead. */
   prog.mode = SC_TEX_MUL_SHADE;
   t0 = now_ns();
   for (k = 0; k < 2000; k++)
   {
      SoftVert a = v[0], b = v[0], c = v[0];
      a.x = (k % 300) * 16; a.y = (k % 200) * 16;
      b.x = a.x + 64; b.y = a.y; c.x = a.x; c.y = a.y + 64;
      soft_draw_tri(&t, &a, &b, &c, &p);
   }
   ns[2] = now_ns() - t0;
   t0 = now_ns();
   for (k = 0; k < 1000; k++)
      (void)now_ns();
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: calib fast %.1f ns/px, generic %.1f ns/px, small tri %.2f us, clock %.0f ns\n",
            (double)ns[0] / (4.0 * SOFT_WIDTH * SOFT_HEIGHT), (double)ns[1] / (4.0 * SOFT_WIDTH * SOFT_HEIGHT),
            (double)ns[2] / 2000e3, (double)(now_ns() - t0) / 1000.0);
}
#endif

void soft_stats_frame(void)
{
#ifdef SOFT_STATS
   static int calibrated;
   if (!calibrated)
   {
      calibrated = 1;
      calibrate();
      memset(stage_ns, 0, sizeof(stage_ns));
      tris = pixels = 0;
   }
#endif

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
      log_cb(RETRO_LOG_INFO, "soft: span px per busy frame: fast %.0f generic %.0f\n",
            span_px[0] / (double)busy_frames, span_px[1] / (double)busy_frames);
   }
   span_px[0] = span_px[1] = 0;
   memset(stage_ns, 0, sizeof(stage_ns));
   busy_frames = 0;
   dl_total_ns = 0;
   tris        = 0;
   pixels      = 0;
   frames      = 0;
}
