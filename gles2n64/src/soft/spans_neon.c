#include "spans.h"
#include "span_pixel.h"

#ifdef __ARM_NEON

#include <arm_neon.h>

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
   int x;

   soft_pixel_inputs(p, &in);

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

      for (j = 0; j < n; j++)
         if (live & (1 << j))
            soft_pixel_finish(p, &in, &dst[x + j], &zb[x + j], zi[j],
                  r[j], g[j], b[j], a[j], f[j], si[j], ti[j]);
   }
}

#endif
