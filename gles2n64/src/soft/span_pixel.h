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

#endif
