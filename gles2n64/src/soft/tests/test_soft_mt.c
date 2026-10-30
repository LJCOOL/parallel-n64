#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "gen_cmds.h"
#include "soft_mt.h"
#include "soft_stats.h"
#include "../../../../libretro/libretro_private.h"

retro_log_printf_t log_cb;

static uint16_t col_a[GW * GH], dep_a[GW * GH], col_b[GW * GH], dep_b[GW * GH];
static SoftQueue queue;

/* Builds one frame immediately into a and queued for b, then replays it with soft_mt_replay. */
static int frame_matches(void)
{
   SoftTarget a, b;
   SoftCombineProgram prog;
   int i;

   for (i = 0; i < GW * GH; i++)
   {
      col_a[i] = col_b[i] = (uint16_t)gen_rnd();
      dep_a[i] = dep_b[i] = (uint16_t)gen_rnd();
   }
   soft_target_init(&a, col_a, dep_a, GW, GH);
   soft_target_init(&b, col_b, dep_b, GW, GH);
   soft_queue_reset(&queue);
   for (i = 0; i < 40; i++)
      gen_command(&a, &b, &queue, &prog);
   soft_mt_replay(&queue, &b);
   return memcmp(col_a, col_b, sizeof col_a) == 0 && memcmp(dep_a, dep_b, sizeof dep_a) == 0;
}

static void test_two_threads_match_immediate(void)
{
   int f, same = 1;
   CHECK(soft_mt_start() == 1);
   for (f = 0; f < 50; f++)
      same &= frame_matches();
   CHECK(same);
}

static void test_fallback_and_restart(void)
{
   soft_mt_stop();
   CHECK(frame_matches());
   CHECK(soft_mt_start() == 1);
   soft_mt_stop();
   soft_mt_stop();
   CHECK(soft_mt_start() == 1);
   CHECK(soft_mt_start() == 1);
   CHECK(frame_matches());
   soft_mt_stop();
}

static void test_stats_lanes_sum(void)
{
   SoftTarget t;
   SoftCombineProgram prog;
   SoftPipe p;
   uint64_t one, two;
   int i;

   soft_target_init(&t, col_b, dep_b, GW, GH);
   soft_queue_reset(&queue);
   for (i = 0; i < 200; i++)
   {
      SoftVert v0 = gen_vert(), v1 = gen_vert(), v2 = gen_vert();
      gen_pipe(&p, &prog);
      p.z_test = 0;
      soft_queue_tri(&queue, &t, &v0, &v1, &v2, &p);
   }
   soft_stats_reset_counts();
   soft_mt_replay(&queue, &t);
   one = soft_stats_pixels();
   CHECK(one > 0);
   CHECK(soft_mt_start() == 1);
   soft_stats_reset_counts();
   soft_mt_replay(&queue, &t);
   two = soft_stats_pixels();
   soft_mt_stop();
   CHECK(one == two);
}

int main(void)
{
   gen_init();
   RUN(test_two_threads_match_immediate);
   RUN(test_fallback_and_restart);
   RUN(test_stats_lanes_sum);
   TEST_EXIT();
}
