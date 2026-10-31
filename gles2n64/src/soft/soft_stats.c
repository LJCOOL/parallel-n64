#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdio.h>
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
static uint64_t tris[2], pixels[2];
static __thread int lane;
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

void soft_stats_set_lane(int l)
{
   lane = l;
}

void soft_stats_count_tri(void)
{
   tris[lane]++;
}

void soft_stats_count_px(int n)
{
   pixels[lane] += (uint64_t)n;
}

uint64_t soft_stats_pixels(void)
{
   return pixels[0] + pixels[1];
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

static uint64_t span_px[2][2];

#define PROG_SLOTS 64

typedef struct
{
   uint8_t sel[2][2][4];
   int cycles;
   uint64_t px;
} ProgCount;

static ProgCount progs[2][PROG_SLOTS];
static int prog_used[2];
static uint64_t prog_other_px[2];

#ifdef SOFT_STATS
void soft_stats_count_path(int generic, int n)
{
   span_px[lane][generic != 0] += (uint64_t)n;
}

static void count_program(int l, const uint8_t sel[2][2][4], int cycles, uint64_t n)
{
   int i;
   for (i = 0; i < prog_used[l]; i++)
      if (progs[l][i].cycles == cycles && !memcmp(progs[l][i].sel, sel, sizeof(progs[l][i].sel)))
      {
         progs[l][i].px += n;
         return;
      }
   if (prog_used[l] == PROG_SLOTS)
   {
      prog_other_px[l] += n;
      return;
   }
   memcpy(progs[l][prog_used[l]].sel, sel, sizeof(progs[l][i].sel));
   progs[l][prog_used[l]].cycles = cycles;
   progs[l][prog_used[l]].px = n;
   prog_used[l]++;
}

void soft_stats_count_program(const void *prog, int n)
{
   const SoftCombineProgram *c = (const SoftCombineProgram*)prog;
   count_program(lane, c->sel, c->cycles, (uint64_t)n);
}

/* Folds lane 1's program counts into lane 0 before reporting. */
static void merge_programs(void)
{
   int i;
   for (i = 0; i < prog_used[1]; i++)
      count_program(0, progs[1][i].sel, progs[1][i].cycles, progs[1][i].px);
   prog_other_px[0] += prog_other_px[1];
   prog_used[1] = 0;
   prog_other_px[1] = 0;
}

static const char *sel_name(int v)
{
   static const char *names[] = { "COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "CENTER", "SCALE",
      "COMB_A", "T0_A", "T1_A", "PRIM_A", "SHADE_A", "ENV_A", "LOD", "PRIM_LOD", "NOISE",
      "K4", "K5", "1", "0" };
   return v < (int)(sizeof(names) / sizeof(names[0])) ? names[v] : "?";
}

/* Logs the busiest generic programs as (A-B)*C+D per cycle, colour then alpha. */
static void log_programs(double busy)
{
   int shown, i, best, cy, ch;
   merge_programs();
   for (shown = 0; shown < 10; shown++)
   {
      char line[256];
      int len;
      best = -1;
      for (i = 0; i < prog_used[0]; i++)
         if (progs[0][i].px && (best < 0 || progs[0][i].px > progs[0][best].px))
            best = i;
      if (best < 0)
         break;
      len = snprintf(line, sizeof(line), "soft: generic #%d %.0f px/busy frame:", shown + 1,
            progs[0][best].px / busy);
      for (cy = 0; cy < progs[0][best].cycles; cy++)
         for (ch = 0; ch < 2; ch++)
         {
            const uint8_t *q = progs[0][best].sel[cy][ch];
            len += snprintf(line + len, sizeof(line) - len, " c%d%s (%s-%s)*%s+%s", cy,
                  ch ? "a" : "rgb", sel_name(q[0]), sel_name(q[1]), sel_name(q[2]), sel_name(q[3]));
         }
      log_cb(RETRO_LOG_INFO, "%s\n", line);
      progs[0][best].px = 0;
   }
   if (prog_other_px[0])
      log_cb(RETRO_LOG_INFO, "soft: generic untracked %.0f px/busy frame\n", prog_other_px[0] / busy);
}

/* One thread's full-screen draw state; each lane owns its buffers, the texture is shared. */
typedef struct
{
   uint16_t col[SOFT_WIDTH * SOFT_HEIGHT], dep[SOFT_WIDTH * SOFT_HEIGHT];
   SoftCombineProgram prog;
   SoftTarget t;
   SoftPipe p;
   SoftVert v[4];
   uint64_t ns[4];
   pthread_t thread;
} CalibLane;

static CalibLane calib_lanes[2];
static volatile int calib_go;

static void calib_lane_init(CalibLane *c, SoftTex *tex)
{
   int i;
   memset(&c->prog, 0, sizeof(c->prog));
   c->prog.cycles = 1; c->prog.uses_tex = 1; c->prog.uses_shade = 1;
   c->prog.sel[0][0][0] = TEXEL0; c->prog.sel[0][0][1] = ZERO; c->prog.sel[0][0][2] = SHADE; c->prog.sel[0][0][3] = ZERO;
   c->prog.sel[0][1][0] = TEXEL0; c->prog.sel[0][1][1] = ZERO; c->prog.sel[0][1][2] = SHADE; c->prog.sel[0][1][3] = ZERO;
   soft_target_init(&c->t, c->col, c->dep, SOFT_WIDTH, SOFT_HEIGHT);
   memset(&c->p, 0, sizeof(c->p));
   c->p.span = SOFT_SPAN_COMBINE; c->p.cull = SOFT_CULL_NONE; c->p.tex = tex; c->p.prog = &c->prog;
   c->p.z_test = 1; c->p.z_update = 1;
   memset(c->v, 0, sizeof(c->v));
   for (i = 0; i < 4; i++)
   {
      c->v[i].x = (i & 1) ? SOFT_WIDTH * 16 : 0; c->v[i].y = (i & 2) ? SOFT_HEIGHT * 16 : 0;
      c->v[i].z = 100; c->v[i].r = c->v[i].g = c->v[i].b = c->v[i].a = 200; c->v[i].invw = 1.0f;
      c->v[i].s = (i & 1) ? 64.0f : 0.0f; c->v[i].t = (i & 2) ? 64.0f : 0.0f;
   }
}

/* Full screen four times per pass: fast, generic, fast filtered, then shade only. */
static void calib_lane_draw(CalibLane *c, int passes)
{
   static const SoftCombineMode modes[4] = { SC_TEX_MUL_SHADE, SC_GENERIC, SC_TEX_MUL_SHADE, SC_SHADE };
   uint64_t t0;
   int k, pass;
   for (pass = 0; pass < passes; pass++)
   {
      c->prog.mode = modes[pass];
      c->prog.uses_tex = modes[pass] != SC_SHADE;
      c->p.filter = pass == 2;
      t0 = now_ns();
      for (k = 0; k < 4; k++)
      {
         memset(c->dep, 0xff, sizeof(c->dep));
         soft_draw_tri(&c->t, &c->v[0], &c->v[1], &c->v[3], &c->p);
         soft_draw_tri(&c->t, &c->v[0], &c->v[3], &c->v[2], &c->p);
      }
      c->ns[pass] = now_ns() - t0;
   }
}

static void *calib_lane_main(void *arg)
{
   while (!calib_go)
      ;
   calib_lane_draw((CalibLane*)arg, 2);
   return NULL;
}

static double calib_px_ns(uint64_t ns)
{
   return (double)ns / (4.0 * SOFT_WIDTH * SOFT_HEIGHT);
}

/* Draws the same full screen on both cores at once; per-pixel slowdown versus solo shows memory contention. */
static void calibrate_contention(double solo_fast, double solo_generic)
{
   CalibLane *other = &calib_lanes[1];
   calib_go = 0;
   if (pthread_create(&other->thread, NULL, calib_lane_main, other) != 0)
      return;
#ifdef __linux__
   {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(1, &set);
      pthread_setaffinity_np(other->thread, sizeof(set), &set);
   }
#endif
   calib_go = 1;
   calib_lane_draw(&calib_lanes[0], 2);
   pthread_join(other->thread, NULL);
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: calib paired fast %.1f/%.1f ns/px (solo %.1f), generic %.1f/%.1f ns/px (solo %.1f)\n",
            calib_px_ns(calib_lanes[0].ns[0]), calib_px_ns(other->ns[0]), solo_fast,
            calib_px_ns(calib_lanes[0].ns[1]), calib_px_ns(other->ns[1]), solo_generic);
}

/* Times synthetic full-screen draws, small triangles and the clock itself on this CPU. */
static void calibrate(void)
{
   static uint16_t trgb[32 * 32];
   static uint8_t ta[32 * 32];
   static uint32_t trgba[32 * 32];
   CalibLane *c = &calib_lanes[0];
   SoftTex tex;
   uint64_t t0, ns2;
   double fast, generic;
   int i, k;

   for (i = 0; i < 32 * 32; i++) { trgb[i] = (uint16_t)(i * 2654435761u >> 16); ta[i] = 255; }
   memset(&tex, 0, sizeof(tex));
   tex.rgb = trgb; tex.a = ta; tex.w = 32; tex.h = 32; tex.mask_s = 31; tex.mask_t = 31;
   tex.rgba = trgba;
   soft_tex_expand(&tex);
   calib_lane_init(&calib_lanes[0], &tex);
   calib_lane_init(&calib_lanes[1], &tex);
   calib_lane_draw(c, 4);
   fast = calib_px_ns(c->ns[0]);
   generic = calib_px_ns(c->ns[1]);
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: calib fast filtered %.1f ns/px, shade only %.1f ns/px\n",
            calib_px_ns(c->ns[2]), calib_px_ns(c->ns[3]));
   c->prog.uses_tex = 1;
   c->p.filter = 0;
   /* 2000 eight-pixel triangles measure per-triangle overhead. */
   c->prog.mode = SC_TEX_MUL_SHADE;
   t0 = now_ns();
   for (k = 0; k < 2000; k++)
   {
      SoftVert a = c->v[0], b = c->v[0], cc = c->v[0];
      a.x = (k % 300) * 16; a.y = (k % 200) * 16;
      b.x = a.x + 64; b.y = a.y; cc.x = a.x; cc.y = a.y + 64;
      soft_draw_tri(&c->t, &a, &b, &cc, &c->p);
   }
   ns2 = now_ns() - t0;
   t0 = now_ns();
   for (k = 0; k < 1000; k++)
      (void)now_ns();
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: calib fast %.1f ns/px, generic %.1f ns/px, small tri %.2f us, clock %.0f ns\n",
            fast, generic, (double)ns2 / 2000e3, (double)(now_ns() - t0) / 1000.0);
   calibrate_contention(fast, generic);
}
#endif

void soft_stats_reset_counts(void)
{
   memset(tris, 0, sizeof(tris));
   memset(pixels, 0, sizeof(pixels));
   memset(span_px, 0, sizeof(span_px));
   memset(prog_used, 0, sizeof(prog_used));
   memset(prog_other_px, 0, sizeof(prog_other_px));
}

void soft_stats_frame(void)
{
#ifdef SOFT_STATS
   static int calibrated;
   if (!calibrated)
   {
      calibrated = 1;
      calibrate();
      memset(stage_ns, 0, sizeof(stage_ns));
      soft_stats_reset_counts();
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
            (unsigned)((tris[0] + tris[1]) / frames), (unsigned)((pixels[0] + pixels[1]) / frames));
   if (log_cb && busy_frames)
   {
      double b = (double)busy_frames * 1e6;
      log_cb(RETRO_LOG_INFO, "soft: per busy frame (%u of %u): dl %.2f pipe %.2f texcrc %.2f decode %.2f raster %.2f present %.2f ms\n",
            busy_frames, frames, dl_total_ns / b, stage_ns[SOFT_T_PIPE] / b,
            stage_ns[SOFT_T_TEXCRC] / b, stage_ns[SOFT_T_DECODE] / b,
            stage_ns[SOFT_T_RASTER] / b, stage_ns[SOFT_T_PRESENT] / b);
      log_cb(RETRO_LOG_INFO, "soft: bands per busy frame: band0 %.2f band1 %.2f wait %.2f ms\n",
            stage_ns[SOFT_T_BAND0] / b, stage_ns[SOFT_T_BAND1] / b, stage_ns[SOFT_T_WAIT] / b);
      log_cb(RETRO_LOG_INFO, "soft: span px per busy frame: fast %.0f generic %.0f\n",
            (span_px[0][0] + span_px[1][0]) / (double)busy_frames,
            (span_px[0][1] + span_px[1][1]) / (double)busy_frames);
#ifdef SOFT_STATS
      log_programs((double)busy_frames);
#endif
   }
   soft_stats_reset_counts();
   memset(stage_ns, 0, sizeof(stage_ns));
   busy_frames = 0;
   dl_total_ns = 0;
   frames      = 0;
}
