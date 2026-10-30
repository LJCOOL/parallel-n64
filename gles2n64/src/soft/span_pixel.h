#ifndef SOFT_SPAN_PIXEL_H
#define SOFT_SPAN_PIXEL_H

/* Per-pixel combine tail shared by the scalar and NEON spans. */

#include "raster.h"
#include "texcache.h"
#include "combine.h"

#include <string.h>

static inline uint16_t pack565(int r, int g, int b)
{
   r = r < 0 ? 0 : (r > 255 ? 255 : r);
   g = g < 0 ? 0 : (g > 255 ? 255 : g);
   b = b < 0 ? 0 : (b > 255 ? 255 : b);
   return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint8_t clamp8(float v)
{
   return (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

static inline void expand565(uint16_t c, uint8_t *out)
{
   out[0] = (uint8_t)(((c >> 11) & 0x1f) << 3 | ((c >> 13) & 7));
   out[1] = (uint8_t)(((c >> 5) & 0x3f) << 2 | ((c >> 9) & 3));
   out[2] = (uint8_t)((c & 0x1f) << 3 | ((c >> 2) & 7));
}

static inline uint16_t blend_pixel(int mode, uint32_t src, uint16_t dst)
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

/* Fills in.prog-independent fields from the pipe. */
static inline void soft_pixel_inputs(const SoftPipe *p, SoftCombineInputs *in)
{
   in->prog = (const SoftCombineProgram*)p->prog;
   memcpy(in->prim, p->prim, 4);
   memcpy(in->env, p->env, 4);
   in->prim_lod = p->prim_lod;
   memset(in->tex0, 0, 4);
   memset(in->tex1, 0, 4);
}

/* Shades one depth-passed pixel at integer texel (si, ti); zi is its clamped depth. */
static inline void soft_pixel_finish(const SoftPipe *p, SoftCombineInputs *in,
      uint16_t *dst, uint16_t *zb, int zi, float r, float g, float b, float a, float f,
      int si, int ti)
{
   uint32_t c;

   in->shade[0] = clamp8(r); in->shade[1] = clamp8(g);
   in->shade[2] = clamp8(b); in->shade[3] = clamp8(a);
   if (in->prog->uses_tex)
   {
      uint16_t texel = soft_tex_sample_i(p->tex, si, ti, &in->tex0[3]);
      expand565(texel, in->tex0);
      memcpy(in->tex1, in->tex0, 4);
   }

   c = soft_combine_eval(in);
   if (p->fog)
      c = soft_fog_apply(c, p->fog_color, clamp8(f));
   if (p->alpha_test && (c >> 24) < p->alpha_ref)
      return;

   *dst = blend_pixel(p->blend, c, *dst);
   if (p->z_test && p->z_update)
      *zb = (uint16_t)zi;
}

static inline int wrap_texel(int i, int n, int mode)
{
   if (mode == SOFT_CLAMP)
      return i < 0 ? 0 : (i >= n ? n - 1 : i);
   if (!(n & (n - 1)))
   {
      if (mode == SOFT_MIRROR)
      {
         i &= 2 * n - 1;
         return i >= n ? 2 * n - 1 - i : i;
      }
      return i & (n - 1);
   }
   if (mode == SOFT_MIRROR)
   {
      i %= 2 * n;
      if (i < 0) i += 2 * n;
      return i >= n ? 2 * n - 1 - i : i;
   }
   i %= n;
   return i < 0 ? i + n : i;
}

static inline int mul8(int x, int y)
{
   return (x * y + 255) >> 8;
}

/* Same result as soft_pixel_finish for every mode except SC_GENERIC, with no calls. */
static inline void soft_pixel_finish_fast(const SoftPipe *p, const SoftCombineInputs *in,
      uint16_t *dst, uint16_t *zb, int zi, float r, float g, float b, float a, float f,
      int si, int ti)
{
   int c[4], sh[4], tx[4] = { 0, 0, 0, 0 }, ch;
   const uint8_t *k;

   sh[0] = clamp8(r); sh[1] = clamp8(g); sh[2] = clamp8(b); sh[3] = clamp8(a);
   if (in->prog->uses_tex)
   {
      const SoftTex *tex = p->tex;
      int i = wrap_texel(ti, tex->h, tex->wrap_t) * tex->w + wrap_texel(si, tex->w, tex->wrap_s);
      uint16_t texel = tex->rgb[i];
      tx[0] = ((texel >> 11) & 0x1f) << 3 | ((texel >> 13) & 7);
      tx[1] = ((texel >> 5) & 0x3f) << 2 | ((texel >> 9) & 3);
      tx[2] = (texel & 0x1f) << 3 | ((texel >> 2) & 7);
      tx[3] = tex->a[i];
   }

   switch (in->prog->mode)
   {
      case SC_SHADE:
         for (ch = 0; ch < 4; ch++) c[ch] = sh[ch];
         break;
      case SC_TEX:
         for (ch = 0; ch < 4; ch++) c[ch] = tx[ch];
         break;
      case SC_TEX_MUL_SHADE:
         for (ch = 0; ch < 4; ch++) c[ch] = mul8(tx[ch], sh[ch]);
         break;
      default:
         k = in->prog->mode == SC_TEX_MUL_PRIM ? in->prim : in->env;
         for (ch = 0; ch < 4; ch++) c[ch] = mul8(tx[ch], k[ch]);
         break;
   }

   if (p->fog)
   {
      int fa = clamp8(f);
      for (ch = 0; ch < 3; ch++)
      {
         int fc = (p->fog_color >> (ch * 8)) & 0xff;
         c[ch] += ((fc - c[ch]) * fa + 127) / 255;
      }
   }
   if (p->alpha_test && c[3] < p->alpha_ref)
      return;

   if (p->blend == SB_OPAQUE)
      *dst = pack565(c[0], c[1], c[2]);
   else if (p->blend != SB_KEEP)
   {
      uint8_t d[3];
      expand565(*dst, d);
      for (ch = 0; ch < 3; ch++)
         c[ch] = p->blend == SB_ADD_FOG_ALPHA ? d[ch] + c[ch] * c[3] / 255
               : d[ch] + (c[ch] - d[ch]) * c[3] / 255;
      *dst = pack565(c[0], c[1], c[2]);
   }
   if (p->z_test && p->z_update)
      *zb = (uint16_t)zi;
}

#endif
