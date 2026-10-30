#include "spans.h"
#include "span_pixel.h"
#include "soft_stats.h"

#ifdef __ARM_NEON

#include <arm_neon.h>
#include <string.h>

static float at(const SoftGrad *g, float fx, float fy)
{
   return g->c + g->dx * fx + g->dy * fy;
}

/* base + dx * k, kept as separate multiply and add to match the scalar span. */
static inline float32x4_t lerp4(float base, float dx, float32x4_t k)
{
   return vaddq_f32(vdupq_n_f32(base), vmulq_f32(vdupq_n_f32(dx), k));
}

/* One reciprocal estimate refined by two Newton steps. */
static inline float32x4_t recip4(float32x4_t w)
{
   float32x4_t e = vrecpeq_f32(w);
   e = vmulq_f32(vrecpsq_f32(w, e), e);
   return vmulq_f32(vrecpsq_f32(w, e), e);
}

/* Matches soft_texel_index: clamp to +-2^30, then floor. */
static inline int32x4_t floor4(float32x4_t v)
{
   int32x4_t i;
   v = vminq_f32(vmaxq_f32(v, vdupq_n_f32(-1073741824.0f)), vdupq_n_f32(1073741824.0f));
   i = vcvtq_s32_f32(v);
   return vaddq_s32(i, vreinterpretq_s32_u32(vcgtq_f32(vcvtq_f32_s32(i), v)));
}

/* Truncating signed division by 255, exact for |v| < 2^16. */
static inline int32x4_t div255(int32x4_t v)
{
   int32x4_t q = vreinterpretq_s32_u32(vshrq_n_u32(
         vmulq_u32(vreinterpretq_u32_s32(vabsq_s32(v)), vdupq_n_u32(0x8081)), 23));
   return vbslq_s32(vcltq_s32(v, vdupq_n_s32(0)), vnegq_s32(q), q);
}

static inline int32x4_t clamp255(int32x4_t v)
{
   return vminq_s32(vmaxq_s32(v, vdupq_n_s32(0)), vdupq_n_s32(255));
}

/* Matches clamp8: clamp to [0, 255] then truncate. */
static inline int32x4_t clamp8_4(float32x4_t v)
{
   return vcvtq_s32_f32(vminq_f32(vmaxq_f32(v, vdupq_n_f32(0.0f)), vdupq_n_f32(255.0f)));
}

static inline int32x4_t mul8_4(int32x4_t x, int32x4_t y)
{
   return vshrq_n_s32(vaddq_s32(vmulq_s32(x, y), vdupq_n_s32(255)), 8);
}

static inline int32x4_t pack565_4(int32x4_t r, int32x4_t g, int32x4_t b)
{
   r = vshlq_n_s32(vshrq_n_s32(clamp255(r), 3), 11);
   g = vshlq_n_s32(vshrq_n_s32(clamp255(g), 2), 5);
   return vorrq_s32(vorrq_s32(r, g), vshrq_n_s32(clamp255(b), 3));
}

/* Four-wide soft_pixel_finish_fast; live holds all-ones lanes that passed depth. */
static void finish4(const SoftPipe *p, const SoftCombineInputs *in, uint16_t *dst,
      uint16_t *zb, int n, uint32x4_t live, int32x4_t zi, float32x4_t vr, float32x4_t vg,
      float32x4_t vb, float32x4_t va, float32x4_t vf, const int32_t *si, const int32_t *ti)
{
   int32x4_t c[4], sh[4], tx[4], d[3];
   uint16_t dd[4] = { 0, 0, 0, 0 }, zz[4] = { 0, 0, 0, 0 };
   int32_t dv[4];
   int ch, j;

   sh[0] = clamp8_4(vr); sh[1] = clamp8_4(vg); sh[2] = clamp8_4(vb); sh[3] = clamp8_4(va);
   if (in->prog->uses_tex)
   {
      const SoftTex *tex = p->tex;
      int32_t t565[4], ta[4];
      int32x4_t t;
      for (j = 0; j < 4; j++)
      {
         int i = wrap_texel(ti[j], tex->h, tex->wrap_t) * tex->w
               + wrap_texel(si[j], tex->w, tex->wrap_s);
         t565[j] = tex->rgb[i];
         ta[j] = tex->a[i];
      }
      t = vld1q_s32(t565);
      tx[0] = vorrq_s32(vshlq_n_s32(vandq_s32(vshrq_n_s32(t, 11), vdupq_n_s32(0x1f)), 3),
            vandq_s32(vshrq_n_s32(t, 13), vdupq_n_s32(7)));
      tx[1] = vorrq_s32(vshlq_n_s32(vandq_s32(vshrq_n_s32(t, 5), vdupq_n_s32(0x3f)), 2),
            vandq_s32(vshrq_n_s32(t, 9), vdupq_n_s32(3)));
      tx[2] = vorrq_s32(vshlq_n_s32(vandq_s32(t, vdupq_n_s32(0x1f)), 3),
            vandq_s32(vshrq_n_s32(t, 2), vdupq_n_s32(7)));
      tx[3] = vld1q_s32(ta);
   }
   else
      tx[0] = tx[1] = tx[2] = tx[3] = vdupq_n_s32(0);

   for (ch = 0; ch < 4; ch++)
      switch (in->prog->mode)
      {
         case SC_SHADE:         c[ch] = sh[ch]; break;
         case SC_TEX:           c[ch] = tx[ch]; break;
         case SC_TEX_MUL_SHADE: c[ch] = mul8_4(tx[ch], sh[ch]); break;
         case SC_TEX_MUL_PRIM:  c[ch] = mul8_4(tx[ch], vdupq_n_s32(in->prim[ch])); break;
         default:               c[ch] = mul8_4(tx[ch], vdupq_n_s32(in->env[ch])); break;
      }

   if (p->fog)
   {
      int32x4_t fa = clamp8_4(vf);
      for (ch = 0; ch < 3; ch++)
      {
         int32x4_t fc = vdupq_n_s32((p->fog_color >> (ch * 8)) & 0xff);
         c[ch] = vaddq_s32(c[ch], div255(vaddq_s32(
               vmulq_s32(vsubq_s32(fc, c[ch]), fa), vdupq_n_s32(127))));
      }
   }
   if (p->alpha_test)
      live = vandq_u32(live, vcgeq_s32(c[3], vdupq_n_s32(p->alpha_ref)));

   memcpy(dd, dst, n * sizeof(uint16_t));
   for (j = 0; j < 4; j++)
      dv[j] = dd[j];
   if (p->blend != SB_OPAQUE && p->blend != SB_KEEP)
   {
      int32x4_t dc = vld1q_s32(dv);
      d[0] = vorrq_s32(vshlq_n_s32(vandq_s32(vshrq_n_s32(dc, 11), vdupq_n_s32(0x1f)), 3),
            vandq_s32(vshrq_n_s32(dc, 13), vdupq_n_s32(7)));
      d[1] = vorrq_s32(vshlq_n_s32(vandq_s32(vshrq_n_s32(dc, 5), vdupq_n_s32(0x3f)), 2),
            vandq_s32(vshrq_n_s32(dc, 9), vdupq_n_s32(3)));
      d[2] = vorrq_s32(vshlq_n_s32(vandq_s32(dc, vdupq_n_s32(0x1f)), 3),
            vandq_s32(vshrq_n_s32(dc, 2), vdupq_n_s32(7)));
      for (ch = 0; ch < 3; ch++)
         c[ch] = p->blend == SB_ADD_FOG_ALPHA
               ? vaddq_s32(d[ch], div255(vmulq_s32(c[ch], c[3])))
               : vaddq_s32(d[ch], div255(vmulq_s32(vsubq_s32(c[ch], d[ch]), c[3])));
   }
   if (p->blend != SB_KEEP)
      vst1q_s32(dv, vbslq_s32(live, pack565_4(c[0], c[1], c[2]), vld1q_s32(dv)));
   for (j = 0; j < n; j++)
      dst[j] = (uint16_t)dv[j];

   if (p->z_test && p->z_update)
   {
      memcpy(zz, zb, n * sizeof(uint16_t));
      for (j = 0; j < 4; j++)
         dv[j] = zz[j];
      vst1q_s32(dv, vbslq_s32(live, zi, vld1q_s32(dv)));
      for (j = 0; j < n; j++)
         zb[j] = (uint16_t)dv[j];
   }
}

void soft_span_combine_neon(const SoftTarget *t, const SoftPipe *p,
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
   int bias = p->decal ? SOFT_DECAL_BIAS : 0;
   static const float lane[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
   float32x4_t k = vld1q_f32(lane);
   const float32x4_t four = vdupq_n_f32(4.0f);
   SoftCombineInputs in;
   int fast = prog->mode <= SC_TEX_MUL_ENV && prog->amode == SA_SAME, x;

   soft_pixel_inputs(p, &in);
   soft_stats_count_path(!fast, x1 - x0);

   for (x = x0; x < x1; x += 4, k = vaddq_f32(k, four))
   {
      int32_t zi[4] = { 0, 0, 0, 0 }, si[4] = { 0, 0, 0, 0 }, ti[4] = { 0, 0, 0, 0 };
      float r[4], g[4], b[4], a[4], f[4];
      int n = x1 - x < 4 ? x1 - x : 4, j, live = 0;

      if (p->z_test)
      {
         int32x4_t z = vcvtq_s32_f32(lerp4(z0, tri->z.dx, k));
         z = vsubq_s32(z, vdupq_n_s32(bias));
         z = vminq_s32(vmaxq_s32(z, vdupq_n_s32(0)), vdupq_n_s32(65535));
         vst1q_s32(zi, z);
         for (j = 0; j < n; j++)
            live |= (zi[j] <= zb[x + j]) << j;
         if (!live)
            continue;
      }
      else
         live = (1 << n) - 1;

      if (prog->uses_tex)
      {
         float32x4_t rw = recip4(lerp4(w0, tri->w.dx, k));
         vst1q_s32(si, floor4(vmulq_f32(lerp4(sw0, tri->sw.dx, k), rw)));
         vst1q_s32(ti, floor4(vmulq_f32(lerp4(tw0, tri->tw.dx, k), rw)));
      }
      vst1q_f32(r, lerp4(r0, tri->r.dx, k));
      vst1q_f32(g, lerp4(g0, tri->g.dx, k));
      vst1q_f32(b, lerp4(b0, tri->b.dx, k));
      vst1q_f32(a, lerp4(a0, tri->a.dx, k));
      vst1q_f32(f, lerp4(f0, tri->f.dx, k));

      if (fast)
      {
         static const uint32_t bit[4] = { 1, 2, 4, 8 };
         uint32x4_t lv = vtstq_u32(vdupq_n_u32((uint32_t)live), vld1q_u32(bit));
         finish4(p, &in, &dst[x], &zb[x], n, lv, vld1q_s32(zi),
               vld1q_f32(r), vld1q_f32(g), vld1q_f32(b), vld1q_f32(a), vld1q_f32(f), si, ti);
         continue;
      }
      for (j = 0; j < n; j++)
         if (live & (1 << j))
            soft_pixel_finish(p, &in, &dst[x + j], &zb[x + j], zi[j],
                  r[j], g[j], b[j], a[j], f[j], si[j], ti[j]);
   }
}

#endif
