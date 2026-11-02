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
         /* Fogged and alpha-tested spans take the tight loop too. */
         p.fog = (span >> 6) & 1;
         p.alpha_test = (span >> 7) & 1;
         p.filter = (span >> 5) & 1;
         p.dither = p.filter && (span & 1);
         if (p.dither) p.filter = 0;
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
      if (tight && span % 3 == 0)
      {
         /* Flat depth equal to the buffer exercises the depth test's tie. */
         tri.z.c = 30000.0f;
         tri.z.dx = 0.0f;
         for (i = 0; i < W; i++)
            dep_s[i] = dep_n[i] = (uint16_t)(30000 - (p.decal ? SOFT_DECAL_BIAS : 0));
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

/* TEXEL1 reads tile 1 at tile 0 coordinates mapped through the pipe's scale and offset. */
static void test_texel1_samples_second_tile(void)
{
   static uint16_t rgb0[4], rgb1[16];
   static uint32_t rgba0[4], rgba1[16];
   static uint8_t a0[4], a1[16];
   SoftTex t0, t1;
   SoftCombineProgram prog;
   SoftTarget tg;
   SoftPipe p;
   SoftTri tri;
   int i;

   for (i = 0; i < 4; i++) { rgb0[i] = 0xf800; a0[i] = 255; }
   for (i = 0; i < 16; i++) { rgb1[i] = (uint16_t)(i & 1 ? 0x07e0 : 0x001f); a1[i] = 255; }
   memset(&t0, 0, sizeof(t0)); memset(&t1, 0, sizeof(t1));
   t0.rgb = rgb0; t0.a = a0; t0.rgba = rgba0; t0.w = 2; t0.h = 2;
   t1.rgb = rgb1; t1.a = a1; t1.rgba = rgba1; t1.w = 16; t1.h = 1;
   soft_tex_expand(&t0); soft_tex_expand(&t1);

   memset(&prog, 0, sizeof(prog));
   prog.mode = SC_GENERIC; prog.cycles = 1; prog.uses_tex = 1;
   for (i = 0; i < 2; i++)
   {
      prog.sel[0][i][0] = ZERO; prog.sel[0][i][1] = ZERO;
      prog.sel[0][i][2] = ZERO; prog.sel[0][i][3] = TEXEL1;
   }
   memset(&p, 0, sizeof(p));
   p.prog = &prog; p.tex = &t0; p.tex1 = &t1;
   p.tex1_ks = 2 << 16; p.tex1_kt = 1 << 16; p.tex1_os = 0; p.tex1_ot = 0;

   /* s runs 0.5, 1.5, ..., so tile 1 reads odd texels 1, 3, ...: green. */
   memset(&tri, 0, sizeof(tri));
   tri.w.c = 1.0f; tri.sw.dx = 1.0f;
   memset(col_n, 0, sizeof(col_n)); memset(dep_n, 0, sizeof(dep_n));
   soft_target_init(&tg, col_n, dep_n, W, 1);
   soft_span_fixed(&tg, &p, &tri, 0, 0, 6);
   for (i = 0; i < 6; i++)
      CHECK(col_n[i] == 0x07e0);
}


/* The folded texel blend draws exactly what the generic two-cycle program draws. */
static void test_folded_texel_blend_matches_generic(void)
{
   static uint16_t rgb0[64], rgb1[64], cg[W], cf[W], dg[W], df[W];
   static uint32_t rgba0[64], rgba1[64];
   static uint8_t a0[64], a1[64];
   SoftTex t0, t1;
   SoftCombineProgram gen, fold;
   SoftCombineFold f;
   SoftTarget tg, tf;
   SoftPipe p;
   SoftTri tri, ftri;
   int i, run, misses = 0;

   for (run = 0; run < 200; run++)
   {
      uint8_t shade[4];
      for (i = 0; i < 64; i++)
      {
         rgb0[i] = (uint16_t)rnd(); rgb1[i] = (uint16_t)rnd();
         a0[i] = (uint8_t)rnd(); a1[i] = (uint8_t)rnd();
      }
      memset(&t0, 0, sizeof(t0)); memset(&t1, 0, sizeof(t1));
      t0.rgb = rgb0; t0.a = a0; t0.rgba = rgba0; t0.w = 8; t0.h = 8;
      t1.rgb = rgb1; t1.a = a1; t1.rgba = rgba1; t1.w = 8; t1.h = 8;
      t0.mask_s = t0.mask_t = t1.mask_s = t1.mask_t = 7;
      soft_tex_expand(&t0); soft_tex_expand(&t1);

      memset(&gen, 0, sizeof(gen));
      gen.mode = SC_GENERIC; gen.cycles = 2; gen.uses_tex = gen.uses_tex1 = gen.uses_shade = 1;
      gen.sel[0][0][0] = TEXEL1; gen.sel[0][0][1] = TEXEL0; gen.sel[0][0][2] = ENV_ALPHA; gen.sel[0][0][3] = TEXEL0;
      gen.sel[0][1][0] = ZERO; gen.sel[0][1][1] = ZERO; gen.sel[0][1][2] = ZERO; gen.sel[0][1][3] = ONE;
      gen.sel[1][0][0] = COMBINED; gen.sel[1][0][1] = ZERO; gen.sel[1][0][2] = SHADE; gen.sel[1][0][3] = ZERO;
      gen.sel[1][1][0] = COMBINED; gen.sel[1][1][1] = ZERO; gen.sel[1][1][2] = PRIMITIVE_ALPHA; gen.sel[1][1][3] = ZERO;

      memset(&p, 0, sizeof(p));
      p.tex = &t0; p.tex1 = &t1; p.filter = run & 1;
      p.tex1_ks = p.tex1_kt = 1 << 16; p.tex1_os = 3 << 16;
      p.blend = (run & 2) ? SB_ALPHA : SB_OPAQUE;
      for (i = 0; i < 4; i++) { p.prim[i] = (uint8_t)rnd(); p.env[i] = (uint8_t)rnd(); }
      fold = gen;
      CHECK(soft_combine_fold(&fold, p.prim, p.env, p.prim_lod, &f));
      CHECK(fold.mode == SC_TEX_LERP_MUL_SHADE);

      /* Flat shade, so folding it once matches folding every vertex. */
      memset(&tri, 0, sizeof(tri));
      tri.w.c = 1.0f; tri.sw.dx = 0.37f; tri.tw.c = 2.5f; tri.tw.dx = 0.21f;
      for (i = 0; i < 4; i++) shade[i] = (uint8_t)rnd();
      tri.r.c = shade[0]; tri.g.c = shade[1]; tri.b.c = shade[2]; tri.a.c = shade[3];
      ftri = tri;
      soft_combine_fold_shade(&f, shade);
      ftri.r.c = shade[0]; ftri.g.c = shade[1]; ftri.b.c = shade[2]; ftri.a.c = shade[3];

      for (i = 0; i < W; i++) { cg[i] = cf[i] = (uint16_t)rnd(); dg[i] = df[i] = 0; }
      soft_target_init(&tg, cg, dg, W, 1);
      soft_target_init(&tf, cf, df, W, 1);
      p.prog = &gen;
      soft_span_fixed(&tg, &p, &tri, 0, 0, 40);
      p.prog = &fold;
      soft_span_fixed(&tf, &p, &ftri, 0, 0, 40);
      for (i = 0; i < W; i++)
         misses += !lsb_close(cg[i], cf[i]);
   }
   CHECK(misses == 0);
}

int main(void)
{
   RUN(test_texel1_samples_second_tile);
   RUN(test_folded_texel_blend_matches_generic);
   RUN(test_fixed_matches_scalar_affine);
   RUN(test_fixed_matches_scalar_perspective);
   RUN(test_tight_loop_matches_general_path);
   TEST_EXIT();
}

