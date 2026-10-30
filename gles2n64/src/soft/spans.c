#include "spans.h"
#include "texcache.h"
#include "combine.h"
#include "span_pixel.h"

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
      uint16_t c = soft_tex_sample_f(p->tex, sw * rw, tw * rw, &alpha);

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

/* Attributes are evaluated as base + dx * i so the NEON span matches bit for bit. */
void soft_span_combine(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   const SoftCombineProgram *prog = (const SoftCombineProgram*)p->prog;
   uint16_t *dst = t->color + y * t->width;
   uint16_t *zb  = t->depth + y * t->width;
   float fx = x0 + 0.5f, fy = y + 0.5f;
   float r0 = at(&tri->r, fx, fy), g0 = at(&tri->g, fx, fy), b0 = at(&tri->b, fx, fy);
   float a0 = at(&tri->a, fx, fy), f0 = at(&tri->f, fx, fy);
   float z0 = at(&tri->z, fx, fy), w0 = at(&tri->w, fx, fy);
   float sw0 = at(&tri->sw, fx, fy), tw0 = at(&tri->tw, fx, fy);
   SoftCombineInputs in;
   int x;

   soft_pixel_inputs(p, &in);

   for (x = x0; x < x1; x++)
   {
      float k = (float)(x - x0);
      int zi = 0, si = 0, ti = 0;

      if (p->z_test)
      {
         zi = (int)(z0 + tri->z.dx * k);
         if (p->decal)
            zi -= SOFT_DECAL_BIAS;
         zi = zi < 0 ? 0 : (zi > 65535 ? 65535 : zi);
         if (zi > zb[x])
            continue;
      }
      if (prog->uses_tex)
      {
         float rw = 1.0f / (w0 + tri->w.dx * k);
         si = soft_texel_index((sw0 + tri->sw.dx * k) * rw);
         ti = soft_texel_index((tw0 + tri->tw.dx * k) * rw);
      }
      soft_pixel_finish(p, &in, &dst[x], &zb[x], zi,
            r0 + tri->r.dx * k, g0 + tri->g.dx * k, b0 + tri->b.dx * k,
            a0 + tri->a.dx * k, f0 + tri->f.dx * k, si, ti);
   }
}
