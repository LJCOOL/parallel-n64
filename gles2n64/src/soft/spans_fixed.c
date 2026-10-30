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

/* Integer span: 16.16 colour, texture and depth steps; inlined once per constant mode. */
static inline __attribute__((always_inline)) void span_mode(const SoftTarget *t,
      const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1, const int mode,
      const int amode_const)
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
   int bias = p->decal ? SOFT_DECAL_BIAS : 0, uses_tex = prog->uses_tex;
   /* A negative amode_const reads the alpha mode at run time. */
   const int amode = mode == SC_GENERIC ? SA_SAME : (amode_const < 0 ? (int)prog->amode : amode_const);
   int pow2 = 0, ms = 0, mt = 0, x, ch;
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

      if (p->z_test)
      {
         zi = (z_fast ? (int)(z32 >> 16) : (int)(z >> 16)) - bias;
         zi = zi < 0 ? 0 : (zi > 65535 ? 65535 : zi);
         if (zi > zb[x])
            goto next;
      }

      sh[0] = sat8(r); sh[1] = sat8(g); sh[2] = sat8(b); sh[3] = sat8(a);
      if (uses_tex)
      {
         int si = s >> 16, ti = tt >> 16, i;
         uint32_t texel;
         if (pow2)
            i = (ti & mt) * tex->w + (si & ms);
         else
            i = wrap_texel(ti, tex->h, tex->wrap_t) * tex->w + wrap_texel(si, tex->w, tex->wrap_s);
         texel = tex->rgba[i];
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

      if (p->fog)
      {
         int fa = sat8(f);
         for (ch = 0; ch < 3; ch++)
         {
            int fc = (p->fog_color >> (ch * 8)) & 0xff;
            c[ch] += ((fc - c[ch]) * fa + 127) / 255;
         }
      }
      if (p->alpha_test && c[3] < p->alpha_ref)
         goto next;

      dst[x] = blend_pixel(p->blend, (uint32_t)c[0] | (uint32_t)c[1] << 8
            | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24, dst[x]);
      if (p->z_test && p->z_update)
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

void soft_span_fixed(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1)
{
   const SoftCombineProgram *prog = (const SoftCombineProgram*)p->prog;
   int key = prog->mode * SA_COUNT + prog->amode;

   /* Dedicated loops for common colour and alpha pairs; others pick alpha per pixel. */
#define PAIR(m, a) case (m) * SA_COUNT + (a): span_mode(t, p, tri, y, x0, x1, m, a); return
   switch (key)
   {
      PAIR(SC_TEX_MUL_SHADE, SA_SAME);
      PAIR(SC_TEX_MUL_SHADE, SA_SHADE);
      PAIR(SC_TEX_MUL_SHADE, SA_ENV);
      PAIR(SC_TEX_MUL_SHADE, SA_TEX);
      PAIR(SC_TEX_MUL_SHADE, SA_TEX_MUL_ENV);
      PAIR(SC_SHADE, SA_SAME);
      PAIR(SC_SHADE, SA_ENV);
      PAIR(SC_TEX, SA_SAME);
      PAIR(SC_TEX, SA_SHADE);
      PAIR(SC_TEX, SA_TEX_MUL_ENV);
      default: break;
   }
#undef PAIR
   switch (prog->mode)
   {
      case SC_SHADE:         span_mode(t, p, tri, y, x0, x1, SC_SHADE, -1); break;
      case SC_TEX:           span_mode(t, p, tri, y, x0, x1, SC_TEX, -1); break;
      case SC_TEX_MUL_SHADE: span_mode(t, p, tri, y, x0, x1, SC_TEX_MUL_SHADE, -1); break;
      case SC_TEX_MUL_PRIM:  span_mode(t, p, tri, y, x0, x1, SC_TEX_MUL_PRIM, -1); break;
      case SC_TEX_MUL_ENV:   span_mode(t, p, tri, y, x0, x1, SC_TEX_MUL_ENV, -1); break;
      default:               span_mode(t, p, tri, y, x0, x1, SC_GENERIC, SA_SAME); break;
   }
}
