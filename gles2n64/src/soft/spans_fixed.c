#include <math.h>

#include "spans.h"
#include "span_pixel.h"
#include "soft_stats.h"

/* Texture coordinates are perspective-correct every SEG pixels and affine between. */
#define SEG_SHIFT 3
#define SEG (1 << SEG_SHIFT)
#define TEX_LIMIT 32767.0f

/* Generic combiner operand rows; each holds four channel bytes. */
enum { R_ZERO, R_ONE, R_COMB, R_TEX0, R_TEX1, R_PRIM, R_SHADE, R_ENV, R_LOD, R_COUNT };

typedef struct
{
   uint8_t off[2][4][4];   /* [cycle][channel][A, B, C, D] byte offsets into the rows */
   int cycles;
} GenericPlan;

static float at(const SoftGrad *g, float fx, float fy)
{
   return g->c + g->dx * fx + g->dy * fy;
}

/* floorf without the libm call; valid while v * 65536 fits in int32. */
static inline int32_t fix16(float v)
{
   float x = v * 65536.0f;
   int32_t i = (int32_t)x;
   return i - ((float)i > x);
}

static inline int32_t tex16(float v)
{
   v = v < -TEX_LIMIT ? -TEX_LIMIT : (v > TEX_LIMIT ? TEX_LIMIT : v);
   return fix16(v);
}

static int sat8(int32_t v16)
{
   int v = v16 >> 16;
   return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static int row_of(int sel, int *alpha)
{
   *alpha = 0;
   switch (sel)
   {
      case COMBINED_ALPHA:  *alpha = 1; return R_COMB;
      case TEXEL0_ALPHA:    *alpha = 1; return R_TEX0;
      case TEXEL1_ALPHA:    *alpha = 1; return R_TEX1;
      case PRIMITIVE_ALPHA: *alpha = 1; return R_PRIM;
      case SHADE_ALPHA:     *alpha = 1; return R_SHADE;
      case ENV_ALPHA:       *alpha = 1; return R_ENV;
      case COMBINED:        return R_COMB;
      case TEXEL0:          return R_TEX0;
      case TEXEL1:          return R_TEX1;
      case PRIMITIVE:       return R_PRIM;
      case SHADE:           return R_SHADE;
      case ENVIRONMENT:     return R_ENV;
      case PRIM_LOD_FRAC:   return R_LOD;
      case ONE:             return R_ONE;
      default:              return R_ZERO;
   }
}

static void plan_generic(const SoftCombineProgram *prog, GenericPlan *g)
{
   int cy, ch, i, alpha;
   g->cycles = prog->cycles;
   for (cy = 0; cy < prog->cycles; cy++)
      for (ch = 0; ch < 4; ch++)
         for (i = 0; i < 4; i++)
         {
            int row = row_of(prog->sel[cy][ch == 3][i], &alpha);
            g->off[cy][ch][i] = (uint8_t)(row * 4 + (alpha ? 3 : ch));
         }
}

/* Same arithmetic as soft_combine_generic, reading pre-resolved operand offsets. */
static inline void eval_generic(const GenericPlan *g, uint8_t *v, int *c)
{
   int cy, ch;
   for (cy = 0; cy < g->cycles; cy++)
   {
      int out[4];
      for (ch = 0; ch < 4; ch++)
      {
         const uint8_t *o = g->off[cy][ch];
         int x = (((v[o[0]] - v[o[1]]) * v[o[2]] * 257 + 32768) >> 16) + v[o[3]];
         out[ch] = x < 0 ? 0 : (x > 255 ? 255 : x);
      }
      for (ch = 0; ch < 4; ch++)
         v[R_COMB * 4 + ch] = (uint8_t)out[ch];
   }
   for (ch = 0; ch < 4; ch++)
      c[ch] = v[R_COMB * 4 + ch];
}

/* Texture fetch chosen per span: run-time checks, none, or branchless wrap-or-clamp point or filtered. */
enum { TK_RUNTIME, TK_NONE, TK_POINT, TK_FILTER };

static int tight_enabled = 1;

void soft_span_fixed_set_tight(int on)
{
   tight_enabled = on;
}

/* Wraps a power-of-two axis when and is size - 1, clamps when and is -1. */
static inline int tight_axis(int i, int and_mask, int max)
{
   i &= and_mask;
   return i < 0 ? 0 : (i > max ? max : i);
}

#ifdef __ARM_NEON
#include <arm_neon.h>

/* Four lanes of a 16.16 shade channel, saturated to [0, 255]. */
static inline __attribute__((always_inline)) uint32x4_t quad_shade(int32_t v, int32_t dv, uint32x4_t k)
{
   int32x4_t l = vreinterpretq_s32_u32(vmlaq_n_u32(vdupq_n_u32((uint32_t)v), k, (uint32_t)dv));
   l = vminq_s32(vmaxq_s32(vshrq_n_s32(l, 16), vdupq_n_s32(0)), vdupq_n_s32(255));
   return vreinterpretq_u32_s32(l);
}

static inline __attribute__((always_inline)) uint32x4_t quad_mul8(uint32x4_t x, uint32x4_t y)
{
   return vshrq_n_u32(vaddq_u32(vmulq_u32(x, y), vdupq_n_u32(255)), 8);
}

/* Four opaque pixels matching the scalar loop exactly; texels are fetched per lane. */
static inline __attribute__((always_inline)) void quad_opaque(const SoftPipe *p, const uint32_t *rgba,
      int width, int sand, int tand, int ms, int mt, const int mode, const int tk, int z_test, int z_write,
      int bias, uint16_t *dst, uint16_t *zb, const int32_t *rgb, const int32_t *drgb,
      uint32_t z32, uint32_t dz32, int32_t s, int32_t ds, int32_t tt, int32_t dt)
{
   static const uint32_t lanes[4] = { 0, 1, 2, 3 };
   const uint32x4_t k = vld1q_u32(lanes);
   uint32x4_t c[3], pass = vdupq_n_u32(~0u), px;
   int32x4_t zi = vdupq_n_s32(0);
   int ch, i;

   if (z_test)
   {
      zi = vreinterpretq_s32_u32(vshrq_n_u32(vmlaq_n_u32(vdupq_n_u32(z32), k, dz32), 16));
      zi = vminq_s32(vmaxq_s32(vsubq_s32(zi, vdupq_n_s32(bias)), vdupq_n_s32(0)), vdupq_n_s32(65535));
      pass = vcleq_u32(vreinterpretq_u32_s32(zi), vmovl_u16(vld1_u16(zb)));
      if (!(vgetq_lane_u32(pass, 0) | vgetq_lane_u32(pass, 1) | vgetq_lane_u32(pass, 2) | vgetq_lane_u32(pass, 3)))
         return;
   }
   if (tk != TK_NONE)
   {
      uint32_t texel[4];
      uint32x4_t tv;
      for (i = 0; i < 4; i++)
      {
         int32_t ls = (int32_t)((uint32_t)s + (uint32_t)ds * i), lt = (int32_t)((uint32_t)tt + (uint32_t)dt * i);
         int si = ls >> 16, ti = lt >> 16;
         if (tk == TK_FILTER)
            texel[i] = soft_filter3_at(rgba, width, tight_axis(si, sand, ms), tight_axis(si + 1, sand, ms),
                  tight_axis(ti, tand, mt), tight_axis(ti + 1, tand, mt), (ls >> 11) & 31, (lt >> 11) & 31);
         else
            texel[i] = rgba[tight_axis(ti, tand, mt) * width + tight_axis(si, sand, ms)];
      }
      tv = vld1q_u32(texel);
      for (ch = 0; ch < 3; ch++)
      {
         uint32x4_t t8 = vandq_u32(vshlq_u32(tv, vdupq_n_s32(-8 * ch)), vdupq_n_u32(0xff));
         if (mode == SC_TEX)
            c[ch] = t8;
         else if (mode == SC_TEX_MUL_SHADE)
            c[ch] = quad_mul8(t8, quad_shade(rgb[ch], drgb[ch], k));
         else if (mode == SC_TEX_MUL_PRIM)
            c[ch] = quad_mul8(t8, vdupq_n_u32(p->prim[ch]));
         else
            c[ch] = quad_mul8(t8, vdupq_n_u32(p->env[ch]));
      }
   }
   else
      for (ch = 0; ch < 3; ch++)
         c[ch] = quad_shade(rgb[ch], drgb[ch], k);

   px = vorrq_u32(vorrq_u32(vshlq_n_u32(vshrq_n_u32(c[0], 3), 11), vshlq_n_u32(vshrq_n_u32(c[1], 2), 5)),
         vshrq_n_u32(c[2], 3));
   {
      uint16x4_t m = vmovn_u32(pass);
      vst1_u16(dst, vbsl_u16(m, vmovn_u32(px), vld1_u16(dst)));
      if (z_write)
         vst1_u16(zb, vbsl_u16(m, vmovn_u32(vreinterpretq_u32_s32(zi)), vld1_u16(zb)));
   }
}
#endif

/* Integer span: 16.16 colour, texture and depth steps; inlined once per constant mode. */
static inline __attribute__((always_inline)) void span_mode(const SoftTarget *t,
      const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1, const int mode,
      const int amode_const, const int simple, const int tk)
{
   const SoftCombineProgram *prog = (const SoftCombineProgram*)p->prog;
   const SoftTex *tex = p->tex;
   uint16_t *dst = t->color + y * t->width;
   uint16_t *zb  = t->depth + y * t->width;
   float fx = x0 + 0.5f, fy = y + 0.5f;
   float w0 = at(&tri->w, fx, fy), sw0 = at(&tri->sw, fx, fy), tw0 = at(&tri->tw, fx, fy);
   int32_t r = fix16(at(&tri->r, fx, fy)), dr = fix16(tri->r.dx);
   int32_t g = fix16(at(&tri->g, fx, fy)), dg = fix16(tri->g.dx);
   int32_t b = fix16(at(&tri->b, fx, fy)), db = fix16(tri->b.dx);
   int32_t a = fix16(at(&tri->a, fx, fy)), da = fix16(tri->a.dx);
   int32_t f = fix16(at(&tri->f, fx, fy)), df = fix16(tri->f.dx);
   int64_t z = (int64_t)floor((double)at(&tri->z, fx, fy) * 65536.0);
   int64_t dz = (int64_t)floor((double)tri->z.dx * 65536.0);
   int32_t s = 0, tt = 0, ds = 0, dt = 0, seg_s = 0, seg_t = 0;
   int bias = p->decal ? SOFT_DECAL_BIAS : 0;
   const int uses_tex = tk == TK_POINT || tk == TK_FILTER || (tk == TK_RUNTIME && prog->uses_tex);
   const uint32_t *rgba = uses_tex ? tex->rgba : NULL;
   int width = 0, sand = 0, tand = 0;
   /* A negative amode_const reads the alpha mode at run time. */
   const int amode = mode == SC_GENERIC ? SA_SAME : (amode_const < 0 ? (int)prog->amode : amode_const);
   int pow2 = 0, ms = 0, mt = 0, x, ch, filter = p->filter;
   /* Flags read once; simple 1 is opaque, 2 blended, both with no fog or alpha test. */
   const int z_test = p->z_test, z_write = p->z_test && p->z_update;
   const int fog = !simple && p->fog, alpha_test = !simple && p->alpha_test;
   const int blend = simple == 1 ? SB_OPAQUE : p->blend;
   /* Rows whose depth stays in [0, 65536) step unsigned 32-bit; others need 64-bit. */
   int64_t z_end = z + dz * (x1 - x0 - 1);
   int z_fast = z >= 0 && z < (65536LL << 16) && z_end >= 0 && z_end < (65536LL << 16);
   uint32_t z32 = (uint32_t)z, dz32 = (uint32_t)dz;
   GenericPlan plan;
   uint8_t v[R_COUNT * 4];

   if (uses_tex)
   {
      float rw = 1.0f / w0;
      s  = tex16(sw0 * rw);
      tt = tex16(tw0 * rw);
      pow2 = tex->wrap_s == SOFT_WRAP && tex->wrap_t == SOFT_WRAP
          && !(tex->w & (tex->w - 1)) && !(tex->h & (tex->h - 1));
      ms = tex->w - 1;
      mt = tex->h - 1;
      width = tex->w;
      sand = tex->wrap_s == SOFT_CLAMP ? -1 : ms;
      tand = tex->wrap_t == SOFT_CLAMP ? -1 : mt;
   }
   if (mode == SC_GENERIC)
   {
      plan_generic(prog, &plan);
      memset(v, 0, sizeof(v));
      for (ch = 0; ch < 4; ch++)
      {
         v[R_ONE * 4 + ch]  = 255;
         v[R_PRIM * 4 + ch] = p->prim[ch];
         v[R_ENV * 4 + ch]  = p->env[ch];
         v[R_LOD * 4 + ch]  = p->prim_lod;
      }
   }

   soft_stats_count_path(mode == SC_GENERIC, x1 - x0);
   if (mode == SC_GENERIC)
      soft_stats_count_program(prog, x1 - x0);

   for (x = x0; x < x1; x++)
   {
      int c[4], tx[4] = { 0, 0, 0, 0 }, sh[4], zi = 0, k = x - x0;

      if (uses_tex && !(k & (SEG - 1)))
      {
         /* Exact coordinates at this segment end; step affinely to the next. */
         float ke = (float)(k + SEG), rw = 1.0f / (w0 + tri->w.dx * ke);
         int32_t se = tex16((sw0 + tri->sw.dx * ke) * rw), te = tex16((tw0 + tri->tw.dx * ke) * rw);
         if (k)
         {
            s  = seg_s;
            tt = seg_t;
         }
         seg_s = se;
         seg_t = te;
         ds = (se - s) >> SEG_SHIFT;
         dt = (te - tt) >> SEG_SHIFT;
      }

#ifdef __ARM_NEON
      /* Opaque tight spans draw four pixels per step; quads never straddle a texture segment. */
      if (simple == 1 && tk != TK_RUNTIME && z_fast && !(k & 3) && x + 4 <= x1)
      {
         const int32_t rgb[3] = { r, g, b }, drgb[3] = { dr, dg, db };
         quad_opaque(p, rgba, width, sand, tand, ms, mt, mode, tk, z_test, z_write, bias,
               dst + x, zb + x, rgb, drgb, z32, dz32, s, ds, tt, dt);
         r += 4 * dr; g += 4 * dg; b += 4 * db; a += 4 * da; f += 4 * df;
         z32 += 4 * dz32; s += 4 * ds; tt += 4 * dt;
         x += 3;
         continue;
      }
#endif
      if (z_test)
      {
         zi = (z_fast ? (int)(z32 >> 16) : (int)(z >> 16)) - bias;
         zi = zi < 0 ? 0 : (zi > 65535 ? 65535 : zi);
         if (zi > zb[x])
            goto next;
      }

      sh[0] = sat8(r); sh[1] = sat8(g); sh[2] = sat8(b); sh[3] = sat8(a);
      if (uses_tex)
      {
         int si = s >> 16, ti = tt >> 16;
         uint32_t texel;
         if (tk == TK_FILTER)
            texel = soft_filter3_at(rgba, width, tight_axis(si, sand, ms), tight_axis(si + 1, sand, ms),
                  tight_axis(ti, tand, mt), tight_axis(ti + 1, tand, mt), (s >> 11) & 31, (tt >> 11) & 31);
         else if (tk == TK_POINT)
            texel = rgba[tight_axis(ti, tand, mt) * width + tight_axis(si, sand, ms)];
         else if (filter && pow2)
            texel = soft_filter3_at(tex->rgba, tex->w, si & ms, (si + 1) & ms,
                  ti & mt, (ti + 1) & mt, (s >> 11) & 31, (tt >> 11) & 31);
         else if (filter)
            texel = soft_tex_filter3(tex, s, tt);
         else if (pow2)
            texel = tex->rgba[(ti & mt) * tex->w + (si & ms)];
         else
            texel = tex->rgba[wrap_texel(ti, tex->h, tex->wrap_t) * tex->w
                  + wrap_texel(si, tex->w, tex->wrap_s)];
         tx[0] = texel & 0xff;
         tx[1] = (texel >> 8) & 0xff;
         tx[2] = (texel >> 16) & 0xff;
         tx[3] = texel >> 24;
      }

      switch (mode)
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
         case SC_TEX_MUL_PRIM:
            for (ch = 0; ch < 4; ch++) c[ch] = mul8(tx[ch], p->prim[ch]);
            break;
         case SC_TEX_MUL_ENV:
            for (ch = 0; ch < 4; ch++) c[ch] = mul8(tx[ch], p->env[ch]);
            break;
         default:
            for (ch = 0; ch < 4; ch++)
            {
               v[R_TEX0 * 4 + ch]  = v[R_TEX1 * 4 + ch] = (uint8_t)tx[ch];
               v[R_SHADE * 4 + ch] = (uint8_t)sh[ch];
               v[R_COMB * 4 + ch]  = 0;
            }
            eval_generic(&plan, v, c);
            break;
      }

      switch (amode)
      {
         case SA_SAME:          break;
         case SA_SHADE:         c[3] = sh[3]; break;
         case SA_ENV:           c[3] = p->env[3]; break;
         case SA_PRIM:          c[3] = p->prim[3]; break;
         case SA_TEX:           c[3] = tx[3]; break;
         case SA_TEX_MUL_SHADE: c[3] = mul8(tx[3], sh[3]); break;
         case SA_TEX_MUL_ENV:   c[3] = mul8(tx[3], p->env[3]); break;
         default:               c[3] = mul8(tx[3], p->prim[3]); break;
      }

      if (fog)
      {
         int fa = sat8(f);
         for (ch = 0; ch < 3; ch++)
         {
            int fc = (p->fog_color >> (ch * 8)) & 0xff;
            c[ch] += ((fc - c[ch]) * fa + 127) / 255;
         }
      }
      if (alpha_test && c[3] < p->alpha_ref)
         goto next;

      /* Every mode leaves c in [0, 255], so opaque packing needs no clamp. */
      if (blend == SB_OPAQUE)
         dst[x] = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3);
      else
         dst[x] = blend_pixel(blend, (uint32_t)c[0] | (uint32_t)c[1] << 8
               | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24, dst[x]);
      if (z_write)
         zb[x] = (uint16_t)zi;
next:
      r += dr; g += dg; b += db; a += da; f += df;
      if (z_fast)
         z32 += dz32;
      else
         z += dz;
      s += ds; tt += dt;
   }
}

/* Tight loops per texture kind; opaque drops the alpha mode, blended keeps it. */
static inline __attribute__((always_inline)) void tight_opaque(const SoftTarget *t,
      const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1, const int m, int tk)
{
   if (tk == TK_POINT) span_mode(t, p, tri, y, x0, x1, m, SA_SAME, 1, TK_POINT);
   else if (tk == TK_FILTER) span_mode(t, p, tri, y, x0, x1, m, SA_SAME, 1, TK_FILTER);
   else span_mode(t, p, tri, y, x0, x1, m, SA_SAME, 1, TK_NONE);
}

static inline __attribute__((always_inline)) void tight_blended(const SoftTarget *t,
      const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1, const int m, int tk)
{
   if (tk == TK_POINT) span_mode(t, p, tri, y, x0, x1, m, -1, 2, TK_POINT);
   else if (tk == TK_FILTER) span_mode(t, p, tri, y, x0, x1, m, -1, 2, TK_FILTER);
   else span_mode(t, p, tri, y, x0, x1, m, -1, 2, TK_NONE);
}

static int tight_axis_ok(int wrap, int n)
{
   return wrap == SOFT_CLAMP || (wrap == SOFT_WRAP && !(n & (n - 1)));
}

void soft_span_fixed(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   const SoftCombineProgram *prog = (const SoftCombineProgram*)p->prog;
   const int nofx = !p->fog && !p->alpha_test, opaque = p->blend == SB_OPAQUE;
   const int simple = opaque && nofx;
   const SoftTex *tex = p->tex;
   int tk = TK_RUNTIME;
   /* Opaque spans never read alpha, so the alpha mode and alpha-only texture use drop out. */
   if (tight_enabled && nofx && prog->mode != SC_GENERIC)
   {
      if (opaque ? prog->mode == SC_SHADE : !prog->uses_tex)
         tk = TK_NONE;
      else if (tight_axis_ok(tex->wrap_s, tex->w) && tight_axis_ok(tex->wrap_t, tex->h))
         tk = p->filter ? TK_FILTER : TK_POINT;
   }
#ifdef SOFT_STATS
   {
      unsigned miss = 0;
      if (tk != TK_RUNTIME)
         ;
      else
      {
      if (p->blend != SB_OPAQUE) miss |= 1u << SOFT_MISS_BLEND;
      if (p->fog) miss |= 1u << SOFT_MISS_FOG;
      if (p->alpha_test) miss |= 1u << SOFT_MISS_ALPHA_TEST;
      if (!simple && prog->mode != SC_GENERIC && prog->amode != SA_SAME) miss |= 1u << SOFT_MISS_SPLIT_ALPHA;
      if (prog->mode == SC_GENERIC) miss |= 1u << SOFT_MISS_GENERIC;
      if (!prog->uses_tex)
         miss |= 1u << SOFT_MISS_UNTEXTURED;
      else
      {
         if (tex->wrap_s == SOFT_MIRROR || tex->wrap_t == SOFT_MIRROR) miss |= 1u << SOFT_MISS_MIRROR;
         if ((tex->wrap_s == SOFT_WRAP && (tex->w & (tex->w - 1)))
               || (tex->wrap_t == SOFT_WRAP && (tex->h & (tex->h - 1))))
            miss |= 1u << SOFT_MISS_NPOT;
      }
      }
      soft_stats_count_variant(miss, x1 - x0);
   }
#endif
   /* Alpha following the colour mode gets its own loop; split alpha is chosen per pixel. */
#define MODE(m) case m: \
      if (tk != TK_RUNTIME && opaque) tight_opaque(t, p, tri, y, x0, x1, m, tk); \
      else if (tk != TK_RUNTIME) tight_blended(t, p, tri, y, x0, x1, m, tk); \
      else if (prog->amode == SA_SAME && simple) span_mode(t, p, tri, y, x0, x1, m, SA_SAME, 1, TK_RUNTIME); \
      else if (prog->amode == SA_SAME) span_mode(t, p, tri, y, x0, x1, m, SA_SAME, 0, TK_RUNTIME); \
      else span_mode(t, p, tri, y, x0, x1, m, -1, 0, TK_RUNTIME); \
      break
   switch (prog->mode)
   {
      MODE(SC_SHADE);
      MODE(SC_TEX);
      MODE(SC_TEX_MUL_SHADE);
      MODE(SC_TEX_MUL_PRIM);
      MODE(SC_TEX_MUL_ENV);
      default:
         if (simple) span_mode(t, p, tri, y, x0, x1, SC_GENERIC, SA_SAME, 1, TK_RUNTIME);
         else span_mode(t, p, tri, y, x0, x1, SC_GENERIC, SA_SAME, 0, TK_RUNTIME);
         break;
   }
#undef MODE
}
