#include "raster.h"

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

void soft_span_flat(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   const SoftVert *v = tri->v[0];
   uint16_t c = (uint16_t)(((v->r >> 3) << 11) | ((v->g >> 2) << 5) | (v->b >> 3));
   uint16_t *dst = t->color + y * t->width;
   int x;
   (void)p;
   for (x = x0; x < x1; x++)
      dst[x] = c;
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

void soft_draw_tri(SoftTarget *t, const SoftVert *v0, const SoftVert *v1,
      const SoftVert *v2, const SoftPipe *p)
{
   SoftTri tri;
   Edge e[3];
   int64_t area, minx, miny, maxx, maxy, px, py;
   int x, y, x0, y0, x1, y1;
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

   px = (int64_t)x0 * 16 + 8;
   py = (int64_t)y0 * 16 + 8;
   edge_setup(&e[0], a, b, px, py);
   edge_setup(&e[1], b, c, px, py);
   edge_setup(&e[2], c, a, px, py);

   for (y = y0; y < y1; y++)
   {
      int64_t w0 = e[0].value, w1 = e[1].value, w2 = e[2].value;
      int start = -1, end = -1;

      for (x = x0; x < x1; x++)
      {
         if ((w0 | w1 | w2) >= 0)
         {
            if (start < 0)
               start = x;
            end = x + 1;
         }
         else if (start >= 0)
            break;
         w0 += e[0].step_x;
         w1 += e[1].step_x;
         w2 += e[2].step_x;
      }
      if (start >= 0)
         p->span(t, p, &tri, y, start, end);

      e[0].value += e[0].step_y;
      e[1].value += e[1].step_y;
      e[2].value += e[2].step_y;
   }
}
