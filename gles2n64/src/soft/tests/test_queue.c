#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "gen_cmds.h"

static uint16_t col_a[GW * GH], dep_a[GW * GH], col_b[GW * GH], dep_b[GW * GH];
static SoftQueue queue;

static void start(SoftTarget *a, SoftTarget *b)
{
   int i;
   for (i = 0; i < GW * GH; i++)
   {
      col_a[i] = col_b[i] = (uint16_t)gen_rnd();
      dep_a[i] = dep_b[i] = (uint16_t)gen_rnd();
   }
   soft_target_init(a, col_a, dep_a, GW, GH);
   soft_target_init(b, col_b, dep_b, GW, GH);
   soft_queue_reset(&queue);
}

static void flush(SoftTarget *b)
{
   soft_queue_replay(&queue, b, 0);
   soft_queue_replay(&queue, b, 1);
   soft_queue_reset(&queue);
}

static void test_replay_matches_immediate(void)
{
   SoftTarget a, b;
   SoftCombineProgram prog;
   int i;

   int same = 1;

   /* Compare at checkpoints so a later clear cannot hide an earlier mismatch. */
   start(&a, &b);
   for (i = 1; i <= 300; i++)
   {
      gen_command(&a, &b, &queue, &prog);
      if (i % 25 == 0)
      {
         flush(&b);
         same &= memcmp(col_a, col_b, sizeof col_a) == 0 && memcmp(dep_a, dep_b, sizeof dep_a) == 0;
      }
   }
   CHECK(same);
}

static void test_queue_overflow_flushes_in_order(void)
{
   SoftTarget a, b;
   SoftCombineProgram prog;
   SoftPipe p;
   int i, flushes = 0;

   start(&a, &b);
   for (i = 0; i < 3 * SOFT_QUEUE_MAX; i++)
   {
      SoftVert v0 = gen_vert(), v1 = gen_vert(), v2 = gen_vert();
      if (soft_queue_full(&queue))
      {
         flush(&b);
         flushes++;
      }
      gen_pipe(&p, &prog);
      soft_draw_tri(&a, &v0, &v1, &v2, &p);
      soft_queue_tri(&queue, &b, &v0, &v1, &v2, &p);
   }
   flush(&b);
   CHECK(flushes >= 1);
   CHECK(memcmp(col_a, col_b, sizeof col_a) == 0);
   CHECK(memcmp(dep_a, dep_b, sizeof dep_a) == 0);
}

int main(void)
{
   gen_init();
   RUN(test_replay_matches_immediate);
   RUN(test_queue_overflow_flushes_in_order);
   TEST_EXIT();
}
