#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "raster.h"
#include "spans.h"
#include "texcache.h"
#include "combine.h"

#ifdef __ARM_NEON

#define W 160
#define TW 32

static uint16_t col_s[W], col_n[W], dep_s[W], dep_n[W];
static uint16_t tex_rgb[TW * TW];
static uint8_t tex_a[TW * TW];

static uint32_t seed = 12345;

static uint32_t rnd(void)
{
   seed = seed * 1664525u + 1013904223u;
   return seed >> 8;
}

static float frand(float lo, float hi)
{
   return lo + (hi - lo) * (float)(rnd() & 0xffff) / 65535.0f;
}

static int lsb_close(uint16_t a, uint16_t b)
{
   int dr = (a >> 11) - (b >> 11), dg = ((a >> 5) & 0x3f) - ((b >> 5) & 0x3f);
   int db = (a & 0x1f) - (b & 0x1f);
   return abs(dr) <= 1 && abs(dg) <= 1 && abs(db) <= 1;
}

static void grad(SoftGrad *g, float lo, float hi, float slope)
{
   g->c = frand(lo, hi);
   g->dx = frand(-slope, slope);
   g->dy = 0.0f;
}

static void test_neon_matches_scalar(void)
{
   SoftTex tex;
   SoftCombineProgram prog;
   SoftTarget ts, tn;
   SoftPipe p;
   SoftTri tri;
   int i, span, pixels = 0, colour_misses = 0, depth_misses = 0;

   for (i = 0; i < TW * TW; i++)
   {
      tex_rgb[i] = (uint16_t)rnd();
      tex_a[i] = (uint8_t)rnd();
   }
   tex.rgb = tex_rgb; tex.a = tex_a; tex.w = TW; tex.h = TW;
   tex.mask_s = TW - 1; tex.mask_t = TW - 1;

   for (span = 0; span < 1000; span++)
   {
      int len = 1 + (int)(rnd() % W), x0 = (int)(rnd() % (W - len + 1));

      memset(&prog, 0, sizeof(prog));
      prog.mode = (SoftCombineMode)(span % (SC_GENERIC + 1));
      prog.cycles = 1;
      prog.uses_tex = prog.mode != SC_SHADE && prog.mode != SC_LERP_ENV_PRIM;
      prog.uses_shade = 1;
      prog.sel[0][0][0] = TEXEL0; prog.sel[0][0][1] = ENVIRONMENT;
      prog.sel[0][0][2] = SHADE;  prog.sel[0][0][3] = ENVIRONMENT;
      prog.sel[0][1][0] = TEXEL0; prog.sel[0][1][1] = ZERO;
      prog.sel[0][1][2] = SHADE;  prog.sel[0][1][3] = ZERO;
      if (prog.mode == SC_GENERIC)
         prog.uses_tex = 1;

      tex.wrap_s = (uint8_t)(rnd() % 3);
      tex.wrap_t = (uint8_t)(rnd() % 3);

      memset(&p, 0, sizeof(p));
      p.tex = &tex;
      p.prog = &prog;
      p.z_test = (rnd() & 3) != 0;
      p.z_update = (rnd() & 1);
      p.decal = (rnd() & 7) == 0;
      p.alpha_test = (rnd() & 1);
      p.alpha_ref = (uint8_t)rnd();
      p.fog = (rnd() & 1);
      p.fog_color = rnd();
      p.blend = (int)((span / (SC_GENERIC + 1)) % (SB_KEEP + 1));
      for (i = 0; i < 4; i++)
      {
         p.prim[i] = (uint8_t)rnd();
         p.env[i] = (uint8_t)rnd();
      }

      memset(&tri, 0, sizeof(tri));
      grad(&tri.z, 0.0f, 65535.0f, 400.0f);
      grad(&tri.r, -20.0f, 275.0f, 4.0f);
      grad(&tri.g, -20.0f, 275.0f, 4.0f);
      grad(&tri.b, -20.0f, 275.0f, 4.0f);
      grad(&tri.a, -20.0f, 275.0f, 4.0f);
      grad(&tri.f, -20.0f, 275.0f, 4.0f);
      grad(&tri.w, 0.05f, 2.0f, 0.001f);
      grad(&tri.sw, -200.0f, 200.0f, 2.0f);
      grad(&tri.tw, -200.0f, 200.0f, 2.0f);

      for (i = 0; i < W; i++)
      {
         col_s[i] = col_n[i] = (uint16_t)rnd();
         dep_s[i] = dep_n[i] = (uint16_t)rnd();
      }
      soft_target_init(&ts, col_s, dep_s, W, 1);
      soft_target_init(&tn, col_n, dep_n, W, 1);

      soft_span_combine(&ts, &p, &tri, 0, x0, x0 + len);
      soft_span_combine_neon(&tn, &p, &tri, 0, x0, x0 + len);

      for (i = 0; i < W; i++)
      {
         if (dep_s[i] != dep_n[i])
            depth_misses++;
         if (!lsb_close(col_s[i], col_n[i]))
            colour_misses++;
      }
      pixels += len;
   }
   /* 1/w differs in the last bits, so rare texel-boundary flips are allowed. */
   CHECK(depth_misses == 0);
   CHECK(colour_misses * 1000 <= pixels);
   printf("spans_neon: %d pixels, %d colour misses\n", pixels, colour_misses);
}

int main(void)
{
   RUN(test_neon_matches_scalar);
   TEST_EXIT();
}

#else

int main(void)
{
   return 0;
}

#endif
