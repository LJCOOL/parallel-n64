#include "spans.h"
#include "texcache.h"
#include "combine.h"

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

static uint8_t clamp8(float v)
{
   return (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

static void expand565(uint16_t c, uint8_t *out)
{
   out[0] = (uint8_t)(((c >> 11) & 0x1f) << 3 | ((c >> 13) & 7));
   out[1] = (uint8_t)(((c >> 5) & 0x3f) << 2 | ((c >> 9) & 3));
   out[2] = (uint8_t)((c & 0x1f) << 3 | ((c >> 2) & 7));
}

static uint16_t blend_pixel(int mode, uint32_t src, uint16_t dst)
{
   int a = (int)(src >> 24), ch, out[3];
   uint8_t d[3];

   switch (mode)
   {
      case SB_OPAQUE:
         return pack565(src & 0xff, (src >> 8) & 0xff, (src >> 16) & 0xff);
      case SB_KEEP:
         return dst;
      default:
         break;
   }
   expand565(dst, d);
   for (ch = 0; ch < 3; ch++)
   {
      int s = (src >> (ch * 8)) & 0xff;
      if (mode == SB_ADD_FOG_ALPHA)
         out[ch] = d[ch] + s * a / 255;
      else
         out[ch] = d[ch] + (s - d[ch]) * a / 255;
   }
   return pack565(out[0], out[1], out[2]);
}

void soft_span_combine(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   const SoftCombineProgram *prog = (const SoftCombineProgram*)p->prog;
   uint16_t *dst = t->color + y * t->width;
   uint16_t *zb  = t->depth + y * t->width;
   float fx = x0 + 0.5f, fy = y + 0.5f;
   float r = at(&tri->r, fx, fy), g = at(&tri->g, fx, fy), b = at(&tri->b, fx, fy);
   float a = at(&tri->a, fx, fy), f = at(&tri->f, fx, fy);
   float z = at(&tri->z, fx, fy), w = at(&tri->w, fx, fy);
   float sw = at(&tri->sw, fx, fy), tw = at(&tri->tw, fx, fy);
   SoftCombineInputs in;
   int x;

   in.prog = prog;
   in.prim[0] = p->prim[0]; in.prim[1] = p->prim[1]; in.prim[2] = p->prim[2]; in.prim[3] = p->prim[3];
   in.env[0] = p->env[0]; in.env[1] = p->env[1]; in.env[2] = p->env[2]; in.env[3] = p->env[3];
   in.prim_lod = p->prim_lod;
   in.tex0[0] = in.tex0[1] = in.tex0[2] = in.tex0[3] = 0;

   for (x = x0; x < x1; x++, r += tri->r.dx, g += tri->g.dx, b += tri->b.dx, a += tri->a.dx,
         f += tri->f.dx, z += tri->z.dx, w += tri->w.dx, sw += tri->sw.dx, tw += tri->tw.dx)
   {
      uint32_t c;
      int zi = 0;

      if (p->z_test)
      {
         zi = (int)z;
         if (p->decal)
            zi -= SOFT_DECAL_BIAS;
         zi = zi < 0 ? 0 : (zi > 65535 ? 65535 : zi);
         if (zi > zb[x])
            continue;
      }

      in.shade[0] = clamp8(r); in.shade[1] = clamp8(g);
      in.shade[2] = clamp8(b); in.shade[3] = clamp8(a);
      if (prog->uses_tex)
      {
         float rw = 1.0f / w;
         uint16_t texel = soft_tex_sample_f(p->tex, sw * rw, tw * rw, &in.tex0[3]);
         expand565(texel, in.tex0);
         in.tex1[0] = in.tex0[0]; in.tex1[1] = in.tex0[1];
         in.tex1[2] = in.tex0[2]; in.tex1[3] = in.tex0[3];
      }

      c = soft_combine_eval(&in);
      if (p->fog)
         c = soft_fog_apply(c, p->fog_color, clamp8(f));
      if (p->alpha_test && (c >> 24) < p->alpha_ref)
         continue;

      dst[x] = blend_pixel(p->blend, c, dst[x]);
      if (p->z_test && p->z_update)
         zb[x] = (uint16_t)zi;
   }
}
