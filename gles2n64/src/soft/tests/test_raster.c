#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "raster.h"
#include "spans.h"

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

static uint32_t seed = 777;

static int32_t rnd_coord(int32_t lo, int32_t hi)
{
   seed = seed * 1664525u + 1013904223u;
   return lo + (int32_t)((seed >> 8) % (uint32_t)(hi - lo));
}

/* Edge function with the rasteriser's top-left rule, at pixel centres. */
static int inside_edge(const SoftVert *a, const SoftVert *b, int x, int y)
{
   int64_t dx = (int64_t)b->x - a->x, dy = (int64_t)b->y - a->y;
   int64_t w = dx * ((int64_t)y * 16 + 8 - a->y) - dy * ((int64_t)x * 16 + 8 - a->x);
   int top_left = (dy < 0) || (dy == 0 && dx > 0);
   return top_left ? w >= 0 : w > 0;
}

static void test_coverage_matches_brute_force(void)
{
   SoftTarget t; SoftPipe p;
   int i, x, y, misses = 0;

   for (i = 0; i < 400; i++)
   {
      /* Mix of small, screen-sized and far off-screen slivers. */
      int32_t r = (i % 3 == 0) ? 160 * 16 : (i % 3 == 1) ? 2000 * 16 : 40000 * 16;
      SoftVert v[3];
      const SoftVert *a, *b, *c;
      int64_t area;

      for (x = 0; x < 3; x++)
      {
         v[x] = vert_fixed(rnd_coord(-r, r + W * 16), rnd_coord(-r, r + H * 16));
         /* Half-pixel snapping puts edges exactly on pixel centres. */
         if (i & 4)
         {
            v[x].x &= ~7;
            v[x].y &= ~7;
         }
      }
      setup(&t, &p);
      soft_set_scissor(&t, 3, 2, W - 5, H - 7);
      soft_draw_tri(&t, &v[0], &v[1], &v[2], &p);

      area = ((int64_t)v[1].x - v[0].x) * ((int64_t)v[2].y - v[0].y)
           - ((int64_t)v[2].x - v[0].x) * ((int64_t)v[1].y - v[0].y);
      a = &v[0]; b = area < 0 ? &v[2] : &v[1]; c = area < 0 ? &v[1] : &v[2];
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
         {
            int want = area != 0 && x >= 3 && x < W - 5 && y >= 2 && y < H - 7
                  && inside_edge(a, b, x, y) && inside_edge(b, c, x, y) && inside_edge(c, a, x, y);
            misses += want != hits[y * W + x];
         }
   }
   CHECK(misses == 0);
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

/* Writes values that depend on pixel and prior contents, so order and coverage matter. */
static void stamp_span(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   int x;
   (void)p;
   for (x = x0; x < x1; x++)
   {
      uint16_t *c = &t->color[y * t->width + x], *z = &t->depth[y * t->width + x];
      *c = (uint16_t)(*c * 31 + x + (tri->v[0]->x & 0xff));
      *z = (uint16_t)(*z ^ (y * 7 + x));
   }
}

static void test_bands_compose_to_full_draw(void)
{
   static uint16_t ca[W * H], za[W * H], cb[W * H], zb[W * H], cc[W * H], zc[W * H], c0[W * H], z0[W * H];
   SoftTarget ta, tb, tc; SoftPipe p;
   int i, x, y, untouched = 1;

   for (i = 0; i < W * H; i++)
      ca[i] = cb[i] = cc[i] = (uint16_t)rand(), za[i] = zb[i] = zc[i] = (uint16_t)rand();
   memcpy(c0, ca, sizeof ca); memcpy(z0, za, sizeof za);
   soft_target_init(&ta, ca, za, W, H);
   soft_target_init(&tb, cb, zb, W, H);
   soft_target_init(&tc, cc, zc, W, H);
   memset(&p, 0, sizeof p);
   p.span = stamp_span;
   p.cull = SOFT_CULL_NONE;
   for (i = 0; i < 400; i++)
   {
      int32_t r = (i % 3 == 0) ? 160 * 16 : (i % 3 == 1) ? 2000 * 16 : 40000 * 16;
      SoftVert v[3];
      for (x = 0; x < 3; x++)
         v[x] = vert_fixed(rnd_coord(-r, r + W * 16), rnd_coord(-r, r + H * 16));
      soft_set_scissor(&ta, 3, 2, W - 5, H - 7);
      soft_set_scissor(&tb, 3, 2, W - 5, H - 7);
      soft_draw_tri(&ta, &v[0], &v[1], &v[2], &p);
      soft_draw_tri_band(&tb, &v[0], &v[1], &v[2], &p, 0);
      soft_draw_tri_band(&tb, &v[0], &v[1], &v[2], &p, 1);
      soft_draw_tri_band(&tc, &v[0], &v[1], &v[2], &p, 0);
   }
   CHECK(memcmp(ca, cb, sizeof ca) == 0);
   CHECK(memcmp(za, zb, sizeof za) == 0);
   for (y = 0; y < H; y++)
      if (soft_row_in_band(y, 1))
         for (x = 0; x < W; x++)
            untouched &= cc[y * W + x] == c0[y * W + x] && zc[y * W + x] == z0[y * W + x];
   CHECK(untouched);
}

int main(void)
{
   RUN(test_fill_rule_shared_edge);
   RUN(test_scissor_clamps_to_target);
   RUN(test_reject_degenerate_and_offscreen);
   RUN(test_cull_back);
   RUN(test_coverage_matches_brute_force);
   RUN(test_bands_compose_to_full_draw);
   TEST_EXIT();
}
