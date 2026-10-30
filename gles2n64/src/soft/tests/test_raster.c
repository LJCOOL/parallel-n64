#include <string.h>

#include "test.h"
#include "raster.h"

#define W 160
#define H 120

static uint16_t color[W * H];
static uint16_t depth[W * H];
static uint8_t  hits[W * H];

/* Counts every pixel a span writes, to catch gaps and double writes. */
static void count_span(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   int x;
   (void)p; (void)tri;
   for (x = x0; x < x1; x++)
   {
      CHECK(x >= t->sx0 && x < t->sx1 && y >= t->sy0 && y < t->sy1);
      hits[y * t->width + x]++;
   }
}

static SoftVert vert(int px, int py)
{
   SoftVert v;
   memset(&v, 0, sizeof(v));
   v.x = px * 16;
   v.y = py * 16;
   v.invw = 1.0f;
   return v;
}

static SoftVert vert_fixed(int32_t fx, int32_t fy)
{
   SoftVert v = vert(0, 0);
   v.x = fx;
   v.y = fy;
   return v;
}

static void setup(SoftTarget *t, SoftPipe *p)
{
   memset(hits, 0, sizeof(hits));
   memset(color, 0, sizeof(color));
   soft_target_init(t, color, depth, W, H);
   memset(p, 0, sizeof(*p));
   p->span = count_span;
   p->cull = SOFT_CULL_NONE;
}

static int count_hits(int *doubles)
{
   int i, n = 0;
   *doubles = 0;
   for (i = 0; i < W * H; i++)
   {
      n += hits[i] ? 1 : 0;
      if (hits[i] > 1)
         (*doubles)++;
   }
   return n;
}

static void test_fill_rule_shared_edge(void)
{
   SoftTarget t; SoftPipe p; int doubles, n;
   SoftVert a = vert(0, 0), b = vert(8, 0), c = vert(8, 8), d = vert(0, 8);

   setup(&t, &p);
   soft_draw_tri(&t, &a, &b, &c, &p);
   soft_draw_tri(&t, &a, &c, &d, &p);
   n = count_hits(&doubles);
   CHECK(n == 64);
   CHECK(doubles == 0);

   /* Flat span writes v0's colour; each half keeps its own colour. */
   setup(&t, &p);
   p.span = soft_span_flat;
   a.r = 255; b.r = 255; c.r = 255;
   soft_draw_tri(&t, &a, &b, &c, &p);
   d.b = 255; c.b = 255;
   {
      SoftVert c2 = c, a2 = a;
      c2.r = 0; a2.r = 0; a2.b = 255;
      soft_draw_tri(&t, &a2, &c2, &d, &p);
   }
   CHECK(color[0 * W + 7] == 0xF800);
   CHECK(color[7 * W + 0] == 0x001F);
}

static void test_scissor_clamps_to_target(void)
{
   SoftTarget t; SoftPipe p; int doubles, n;
   SoftVert a = vert(-100, -100), b = vert(400, -100), c = vert(400, 400), d = vert(-100, 400);

   setup(&t, &p);
   soft_set_scissor(&t, -10, -10, 500, 500);
   CHECK(t.sx0 == 0);
   CHECK(t.sy0 == 0);
   CHECK(t.sx1 == 160);
   CHECK(t.sy1 == 120);
   soft_draw_tri(&t, &a, &b, &c, &p);
   soft_draw_tri(&t, &a, &c, &d, &p);
   n = count_hits(&doubles);
   CHECK(n == 19200);
   CHECK(doubles == 0);
}

static void test_reject_degenerate_and_offscreen(void)
{
   SoftTarget t; SoftPipe p; int doubles;
   SoftVert a = vert(0, 0), b = vert(10, 10), c = vert(20, 20);
   SoftVert e = vert(210, 10), f = vert(250, 10), g = vert(230, 50);
   SoftVert h = vert_fixed(1 << 20, 16 * 10), i = vert(10, 100), j = vert(5, 5);

   setup(&t, &p);
   soft_draw_tri(&t, &a, &b, &c, &p);
   CHECK(count_hits(&doubles) == 0);

   setup(&t, &p);
   soft_draw_tri(&t, &e, &f, &g, &p);
   CHECK(count_hits(&doubles) == 0);

   setup(&t, &p);
   soft_set_scissor(&t, 10, 10, 100, 100);
   soft_draw_tri(&t, &j, &h, &i, &p);
   CHECK(count_hits(&doubles) > 0);
   CHECK(doubles == 0);
}

static void test_cull_back(void)
{
   SoftTarget t; SoftPipe p; int doubles;
   /* Screen y points down: a, b, c runs clockwise on screen. */
   SoftVert a = vert(10, 10), b = vert(50, 10), c = vert(10, 50);

   setup(&t, &p);
   p.cull = SOFT_CULL_BACK;
   soft_draw_tri(&t, &a, &b, &c, &p);
   CHECK(count_hits(&doubles) == 0);

   setup(&t, &p);
   p.cull = SOFT_CULL_BACK;
   soft_draw_tri(&t, &a, &c, &b, &p);
   CHECK(count_hits(&doubles) > 0);
}

int main(void)
{
   RUN(test_fill_rule_shared_edge);
   RUN(test_scissor_clamps_to_target);
   RUN(test_reject_degenerate_and_offscreen);
   RUN(test_cull_back);
   TEST_EXIT();
}
