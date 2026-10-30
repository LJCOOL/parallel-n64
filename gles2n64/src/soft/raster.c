#include <math.h>
#include "raster.h"
#include "soft_stats.h"

static int clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

void soft_target_init(SoftTarget *t, uint16_t *color, uint16_t *depth, int w, int h)
{
   t->color  = color;
   t->depth  = depth;
   t->width  = w;
   t->height = h;
   soft_set_scissor(t, 0, 0, w, h);
}

void soft_set_scissor(SoftTarget *t, int x0, int y0, int x1, int y1)
{
   t->sx0 = clampi(x0, 0, t->width);
   t->sy0 = clampi(y0, 0, t->height);
   t->sx1 = clampi(x1, t->sx0, t->width);
   t->sy1 = clampi(y1, t->sy0, t->height);
}

static void grad(SoftGrad *g, const SoftTri *tri, float a0, float a1, float a2)
{
   const SoftVert *v0 = tri->v[0], *v1 = tri->v[1], *v2 = tri->v[2];
   float x0 = v0->x / 16.0f, y0 = v0->y / 16.0f;
   float x1 = v1->x / 16.0f - x0, y1 = v1->y / 16.0f - y0;
   float x2 = v2->x / 16.0f - x0, y2 = v2->y / 16.0f - y0;
   float inv = 1.0f / (x1 * y2 - x2 * y1);
   float d1 = a1 - a0, d2 = a2 - a0;

   g->dx = (d1 * y2 - d2 * y1) * inv;
   g->dy = (d2 * x1 - d1 * x2) * inv;
   g->c  = a0 - g->dx * x0 - g->dy * y0;
}

/* Builds attribute planes for every interpolated value. */
static void soft_tri_setup(SoftTri *tri)
{
   const SoftVert *a = tri->v[0], *b = tri->v[1], *c = tri->v[2];

   grad(&tri->z, tri, a->z, b->z, c->z);
   grad(&tri->r, tri, a->r, b->r, c->r);
   grad(&tri->g, tri, a->g, b->g, c->g);
   grad(&tri->b, tri, a->b, b->b, c->b);
   grad(&tri->a, tri, a->a, b->a, c->a);
   grad(&tri->f, tri, a->fog, b->fog, c->fog);
   grad(&tri->w, tri, a->invw, b->invw, c->invw);
   grad(&tri->sw, tri, a->s * a->invw, b->s * b->invw, c->s * c->invw);
   grad(&tri->tw, tri, a->t * a->invw, b->t * b->invw, c->t * c->invw);
}

/* Edge a->b; top-left edges own their boundary pixels. */
typedef struct
{
   int64_t value;   /* at the bounding box origin pixel centre */
   int64_t step_x, step_y;
} Edge;

static void edge_setup(Edge *e, const SoftVert *a, const SoftVert *b,
      int64_t px, int64_t py)
{
   int64_t dx = (int64_t)b->x - a->x;
   int64_t dy = (int64_t)b->y - a->y;
   int top_left = (dy < 0) || (dy == 0 && dx > 0);

   e->value  = dx * (py - a->y) - dy * (px - a->x);
   e->step_x = -dy * 16;
   e->step_y = dx * 16;
   if (!top_left)
      e->value -= 1;
}

static int64_t floor16(int64_t v)
{
   return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

/* Narrows [*lo, *hi) to the pixels k where w + k * step >= 0. */
static void clip_row(int64_t w, int64_t step, int *lo, int *hi)
{
   int n = *hi, k;
   double q;

   if (step == 0)
   {
      if (w < 0)
         *hi = *lo;
      return;
   }
   if (step > 0)
   {
      if (w >= 0)
         return;
      q = ceil((double)-w / (double)step);
      if (q >= n)
      {
         *lo = *hi;
         return;
      }
      k = (int)q;
      while (k > 0 && w + (int64_t)(k - 1) * step >= 0)
         k--;
      while (k < n && w + (int64_t)k * step < 0)
         k++;
      if (k > *lo)
         *lo = k;
      return;
   }
   if (w < 0)
   {
      *hi = *lo;
      return;
   }
   q = floor((double)w / (double)-step);
   if (q >= n - 1)
      return;
   k = (int)q;
   while (k < n - 1 && w + (int64_t)(k + 1) * step >= 0)
      k++;
   while (k >= 0 && w + (int64_t)k * step < 0)
      k--;
   if (k + 1 < *hi)
      *hi = k + 1;
}

static void draw_tri(SoftTarget *t, const SoftVert *v0, const SoftVert *v1,
      const SoftVert *v2, const SoftPipe *p)
{
   SoftTri tri;
   Edge e[3];
   int64_t area, minx, miny, maxx, maxy, px, py;
   int y, x0, y0, x1, y1;
   const SoftVert *a = v0, *b = v1, *c = v2;

   area = ((int64_t)v1->x - v0->x) * ((int64_t)v2->y - v0->y)
        - ((int64_t)v2->x - v0->x) * ((int64_t)v1->y - v0->y);
   if (area == 0)
      return;
   if (area > 0 && p->cull == SOFT_CULL_BACK)
      return;
   if (area < 0 && p->cull == SOFT_CULL_FRONT)
      return;
   if (area < 0)
   {
      b = v2;
      c = v1;
   }

   minx = v0->x; maxx = v0->x; miny = v0->y; maxy = v0->y;
   if (v1->x < minx) minx = v1->x; if (v1->x > maxx) maxx = v1->x;
   if (v2->x < minx) minx = v2->x; if (v2->x > maxx) maxx = v2->x;
   if (v1->y < miny) miny = v1->y; if (v1->y > maxy) maxy = v1->y;
   if (v2->y < miny) miny = v2->y; if (v2->y > maxy) maxy = v2->y;

   minx = floor16(minx); miny = floor16(miny);
   maxx = floor16(maxx) + 1; maxy = floor16(maxy) + 1;
   x0 = (int)(minx < t->sx0 ? t->sx0 : (minx > t->sx1 ? t->sx1 : minx));
   y0 = (int)(miny < t->sy0 ? t->sy0 : (miny > t->sy1 ? t->sy1 : miny));
   x1 = (int)(maxx > t->sx1 ? t->sx1 : (maxx < x0 ? x0 : maxx));
   y1 = (int)(maxy > t->sy1 ? t->sy1 : (maxy < y0 ? y0 : maxy));
   if (x0 >= x1 || y0 >= y1)
      return;

   tri.v[0] = v0;
   tri.v[1] = v1;
   tri.v[2] = v2;
   soft_tri_setup(&tri);
   soft_stats_count_tri();

   px = (int64_t)x0 * 16 + 8;
   py = (int64_t)y0 * 16 + 8;
   edge_setup(&e[0], a, b, px, py);
   edge_setup(&e[1], b, c, px, py);
   edge_setup(&e[2], c, a, px, py);

   for (y = y0; y < y1; y++)
   {
      int lo = 0, hi = x1 - x0;

      clip_row(e[0].value, e[0].step_x, &lo, &hi);
      clip_row(e[1].value, e[1].step_x, &lo, &hi);
      clip_row(e[2].value, e[2].step_x, &lo, &hi);
      if (lo < hi)
      {
         p->span(t, p, &tri, y, x0 + lo, x0 + hi);
         soft_stats_count_px(hi - lo);
      }

      e[0].value += e[0].step_y;
      e[1].value += e[1].step_y;
      e[2].value += e[2].step_y;
   }
}

void soft_draw_tri(SoftTarget *t, const SoftVert *v0, const SoftVert *v1,
      const SoftVert *v2, const SoftPipe *p)
{
   SOFT_TIME_BEGIN(t0);
   draw_tri(t, v0, v1, v2, p);
   SOFT_TIME_END(SOFT_T_RASTER, t0);
}
