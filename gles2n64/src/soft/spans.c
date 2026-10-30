#include "spans.h"
#include "texcache.h"

static uint16_t pack565(int r, int g, int b)
{
   r = r < 0 ? 0 : (r > 255 ? 255 : r);
   g = g < 0 ? 0 : (g > 255 ? 255 : g);
   b = b < 0 ? 0 : (b > 255 ? 255 : b);
   return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static float at(const SoftGrad *g, float fx, float fy)
{
   return g->c + g->dx * fx + g->dy * fy;
}

/* Depth test and optional write; returns non-zero when the pixel is visible. */
static int depth_pass(const SoftPipe *p, uint16_t *zbuf, float z)
{
   int zi;
   if (!p->z_test)
      return 1;
   zi = (int)z;
   if (p->decal)
      zi -= SOFT_DECAL_BIAS;
   zi = zi < 0 ? 0 : (zi > 65535 ? 65535 : zi);
   if (zi > *zbuf)
      return 0;
   if (p->z_update)
      *zbuf = (uint16_t)zi;
   return 1;
}

void soft_span_flat(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   const SoftVert *v = tri->v[0];
   uint16_t c = pack565(v->r, v->g, v->b);
   uint16_t *dst = t->color + y * t->width;
   int x;
   (void)p;
   for (x = x0; x < x1; x++)
      dst[x] = c;
}

void soft_span_gouraud(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   uint16_t *dst = t->color + y * t->width;
   float fx = x0 + 0.5f, fy = y + 0.5f;
   float r = at(&tri->r, fx, fy), g = at(&tri->g, fx, fy), b = at(&tri->b, fx, fy);
   int x;
   (void)p;
   for (x = x0; x < x1; x++)
   {
      dst[x] = pack565((int)r, (int)g, (int)b);
      r += tri->r.dx; g += tri->g.dx; b += tri->b.dx;
   }
}

void soft_span_gouraud_z(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   uint16_t *dst = t->color + y * t->width;
   uint16_t *zb  = t->depth + y * t->width;
   float fx = x0 + 0.5f, fy = y + 0.5f;
   float r = at(&tri->r, fx, fy), g = at(&tri->g, fx, fy), b = at(&tri->b, fx, fy);
   float z = at(&tri->z, fx, fy);
   int x;
   for (x = x0; x < x1; x++)
   {
      if (depth_pass(p, &zb[x], z))
         dst[x] = pack565((int)r, (int)g, (int)b);
      r += tri->r.dx; g += tri->g.dx; b += tri->b.dx; z += tri->z.dx;
   }
}

/* Shared textured span; shade modulates the texel when requested. */
static void span_tex(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1, int shade)
{
   uint16_t *dst = t->color + y * t->width;
   uint16_t *zb  = t->depth + y * t->width;
   float fx = x0 + 0.5f, fy = y + 0.5f;
   float r = at(&tri->r, fx, fy), g = at(&tri->g, fx, fy), b = at(&tri->b, fx, fy);
   float z = at(&tri->z, fx, fy), w = at(&tri->w, fx, fy);
   float sw = at(&tri->sw, fx, fy), tw = at(&tri->tw, fx, fy);
   int x;

   for (x = x0; x < x1; x++)
   {
      float rw = 1.0f / w;
      uint8_t alpha;
      uint16_t c = soft_tex_sample(p->tex, (int32_t)(sw * rw * 65536.0f),
            (int32_t)(tw * rw * 65536.0f), &alpha);

      if ((!p->alpha_test || alpha >= p->alpha_ref) && depth_pass(p, &zb[x], z))
      {
         if (shade)
         {
            int tr = (c >> 11) << 3, tg = ((c >> 5) & 0x3f) << 2, tb = (c & 0x1f) << 3;
            c = pack565(tr * (int)r >> 8, tg * (int)g >> 8, tb * (int)b >> 8);
         }
         dst[x] = c;
      }
      r += tri->r.dx; g += tri->g.dx; b += tri->b.dx; z += tri->z.dx;
      w += tri->w.dx; sw += tri->sw.dx; tw += tri->tw.dx;
   }
}

void soft_span_tex_z(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   span_tex(t, p, tri, y, x0, x1, 0);
}

void soft_span_tex_gouraud_z(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   span_tex(t, p, tri, y, x0, x1, 1);
}
