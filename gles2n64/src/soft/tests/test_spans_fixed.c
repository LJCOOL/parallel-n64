#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "raster.h"
#include "spans.h"
#include "texcache.h"
#include "combine.h"


#define W 160
#define TW 32

static uint16_t col_s[W], col_n[W], dep_s[W], dep_n[W];
static uint16_t tex_rgb[TW * TW];
static uint32_t tex_rgba[TW * TW];
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

static int depth_close(uint16_t a, uint16_t b)
{
   return abs((int)a - (int)b) <= 1;
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

/* w_lo and w_slope set perspective strength; limits are per-thousand of pixels. */
/* tight 0 compares against the scalar span; 1 compares the tight loop with the general fixed path. */
static void run_fixed_mode(float w_lo, float w_slope, int colour_limit, int exact_limit, int tight)
{
   SoftTex tex;
   SoftCombineProgram prog;
   SoftTarget ts, tn;
   SoftPipe p;
   SoftTri tri;
   int i, span, pixels = 0, colour_misses = 0, exact_misses = 0, depth_misses = 0;

   for (i = 0; i < TW * TW; i++)
   {
      tex_rgb[i] = (uint16_t)rnd();
      tex_a[i] = (uint8_t)rnd();
   }
   tex.rgb = tex_rgb; tex.a = tex_a; tex.w = TW; tex.h = TW;
   tex.mask_s = TW - 1; tex.mask_t = TW - 1;
   tex.rgba = tex_rgba;
   soft_tex_expand(&tex);

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
      if (prog.mode <= SC_TEX_MUL_ENV)
      {
         prog.amode = (SoftAlphaMode)((span / 7) % SA_COUNT);
         if (prog.amode >= SA_TEX)
            prog.uses_tex = 1;
      }
      if (span & 16)
      {
         /* Two cycles with alpha-broadcast selectors and the combined feedback. */
         prog.cycles = 2;
         prog.sel[0][0][2] = TEXEL0_ALPHA;
         prog.sel[1][0][0] = COMBINED;  prog.sel[1][0][1] = ENVIRONMENT;
         prog.sel[1][0][2] = SHADE_ALPHA; prog.sel[1][0][3] = PRIMITIVE;
         prog.sel[1][1][0] = COMBINED;  prog.sel[1][1][1] = ZERO;
         prog.sel[1][1][2] = PRIMITIVE_ALPHA; prog.sel[1][1][3] = COMBINED_ALPHA;
      }

      tex.w = (span & 4) ? 24 : TW;
      tex.h = (span & 8) ? 20 : TW;
      tex.mask_s = tex.w - 1; tex.mask_t = tex.h - 1;
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
      if (tight)
      {
         /* Opaque, unfogged, untested spans are the ones the tight loop takes. */
         p.fog = 0;
         p.alpha_test = 0;
         p.blend = SB_OPAQUE;
         p.filter = (span >> 5) & 1;
      }
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
      grad(&tri.w, w_lo, 2.0f, w_slope);
      grad(&tri.sw, -200.0f, 200.0f, 2.0f);
      grad(&tri.tw, -200.0f, 200.0f, 2.0f);

      for (i = 0; i < W; i++)
      {
         col_s[i] = col_n[i] = (uint16_t)rnd();
         dep_s[i] = dep_n[i] = (uint16_t)rnd();
      }
      soft_target_init(&ts, col_s, dep_s, W, 1);
      soft_target_init(&tn, col_n, dep_n, W, 1);

      if (tight)
      {
         soft_span_fixed_set_tight(0);
         soft_span_fixed(&ts, &p, &tri, 0, x0, x0 + len);
         soft_span_fixed_set_tight(1);
      }
      else
         soft_span_combine(&ts, &p, &tri, 0, x0, x0 + len);
      soft_span_fixed(&tn, &p, &tri, 0, x0, x0 + len);

      for (i = 0; i < W; i++)
      {
         if (!depth_close(dep_s[i], dep_n[i]))
            depth_misses++;
         if (!lsb_close(col_s[i], col_n[i]))
            colour_misses++;
         if (col_s[i] != col_n[i])
            exact_misses++;
         if (tight && dep_s[i] != dep_n[i])
            depth_misses += 1000;
      }
      pixels += len;
   }
   /* An alpha-tested texel flip also skips that pixel's depth write. */
   if (tight)
   {
      CHECK(depth_misses == 0);
      CHECK(exact_misses == 0);
      printf("spans_fixed tight: %d pixels, %d inexact\n", pixels, exact_misses);
      return;
   }
   CHECK(depth_misses * 1000 <= pixels * (colour_limit - 1));
   CHECK(colour_misses * 1000 <= pixels * colour_limit);
   CHECK(exact_misses * 1000 <= pixels * exact_limit);
   printf("spans_fixed: %d pixels, %d colour misses, %d inexact\n", pixels, colour_misses, exact_misses);
}

static void run_fixed(float w_lo, float w_slope, int colour_limit, int exact_limit)
{
   run_fixed_mode(w_lo, w_slope, colour_limit, exact_limit, 0);
}

/* The tight loop must match the general fixed path bit for bit, filtered or not. */
static void test_tight_loop_matches_general_path(void)
{
   run_fixed_mode(0.5f, 0.001f, 0, 0, 1);
}

/* Constant w makes the affine texture steps exact, so only rounding may differ. */
static void test_fixed_matches_scalar_affine(void)
{
   run_fixed(0.05f, 0.0f, 1, 1);
}

/* Perspective within 8-pixel segments is approximated; texel flips stay rare. */
static void test_fixed_matches_scalar_perspective(void)
{
   run_fixed(0.5f, 0.001f, 20, 20);
}

int main(void)
{
   RUN(test_fixed_matches_scalar_affine);
   RUN(test_fixed_matches_scalar_perspective);
   RUN(test_tight_loop_matches_general_path);
   TEST_EXIT();
}

