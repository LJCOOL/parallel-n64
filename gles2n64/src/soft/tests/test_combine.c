#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "combine.h"

static int log_lines;

static void count_log(const char *msg)
{
   (void)msg;
   log_lines++;
}

static DecodedMux mux_of(int csa, int csb, int cm, int ca, int asa, int asb, int am, int aa)
{
   DecodedMux m;
   int i;
   memset(&m, 0, sizeof(m));
   for (i = 0; i < 4; i++)
   {
      m.decode[i].sa = ZERO; m.decode[i].sb = ZERO;
      m.decode[i].m  = ZERO; m.decode[i].a  = ZERO;
   }
   m.decode[0].sa = csa; m.decode[0].sb = csb; m.decode[0].m = cm; m.decode[0].a = ca;
   m.decode[1].sa = asa; m.decode[1].sb = asb; m.decode[1].m = am; m.decode[1].a = aa;
   return m;
}

static void test_classify_tex_mul_shade(void)
{
   DecodedMux m = mux_of(TEXEL0, ZERO, SHADE, ZERO, TEXEL0_ALPHA, ZERO, SHADE_ALPHA, ZERO);
   CHECK(soft_combine_classify(&m) == SC_TEX_MUL_SHADE);
}

static void test_two_cycle_passthrough_uses_first_cycle(void)
{
   SoftCombineProgram prog;
   DecodedMux m = mux_of(TEXEL0, ZERO, SHADE, ZERO, TEXEL0_ALPHA, ZERO, SHADE_ALPHA, ZERO);
   m.decode[2].a = COMBINED;
   m.decode[3].a = COMBINED_ALPHA;
   soft_combine_compile(&m, 1, &prog);
   CHECK(prog.mode == SC_TEX_MUL_SHADE);
   CHECK(prog.cycles == 1);

   /* A second cycle that does real work keeps the generic two-cycle program. */
   m.decode[2].sa = ENVIRONMENT;
   soft_combine_compile(&m, 1, &prog);
   CHECK(prog.mode == SC_GENERIC);
   CHECK(prog.cycles == 2);
}

static void test_alpha_source_splits_from_colour(void)
{
   SoftCombineProgram prog;
   DecodedMux m = mux_of(TEXEL0, ZERO, SHADE, ZERO, ZERO, ZERO, ZERO, SHADE_ALPHA);
   soft_combine_compile(&m, 0, &prog);
   CHECK(prog.mode == SC_TEX_MUL_SHADE && prog.amode == SA_SHADE);
   CHECK(prog.sel[0][1][3] == SHADE_ALPHA);

   m = mux_of(ZERO, ZERO, ZERO, SHADE, ZERO, ZERO, ZERO, ENV_ALPHA);
   soft_combine_compile(&m, 0, &prog);
   CHECK(prog.mode == SC_SHADE && prog.amode == SA_ENV);

   m = mux_of(ZERO, ZERO, ZERO, TEXEL0, TEXEL0_ALPHA, ZERO, ENV_ALPHA, ZERO);
   soft_combine_compile(&m, 0, &prog);
   CHECK(prog.mode == SC_TEX && prog.amode == SA_TEX_MUL_ENV);

   m = mux_of(TEXEL0, ZERO, SHADE, ZERO, TEXEL0_ALPHA, ZERO, SHADE_ALPHA, ZERO);
   soft_combine_compile(&m, 0, &prog);
   CHECK(prog.mode == SC_TEX_MUL_SHADE && prog.amode == SA_SAME);

   /* Alpha outside the fast set keeps the generic program. */
   m = mux_of(TEXEL0, ZERO, SHADE, ZERO, PRIMITIVE_ALPHA, SHADE_ALPHA, TEXEL0_ALPHA, SHADE_ALPHA);
   soft_combine_compile(&m, 0, &prog);
   CHECK(prog.mode == SC_GENERIC);
}

static void test_eval_alpha_modes_match_generic(void)
{
   SoftCombineProgram prog;
   SoftCombineInputs in;
   DecodedMux m;
   static const int asel[][4] = {
      { ZERO, ZERO, ZERO, SHADE_ALPHA }, { ZERO, ZERO, ZERO, ENV_ALPHA },
      { ZERO, ZERO, ZERO, PRIMITIVE_ALPHA }, { ZERO, ZERO, ZERO, TEXEL0_ALPHA },
      { TEXEL0_ALPHA, ZERO, SHADE_ALPHA, ZERO }, { TEXEL0_ALPHA, ZERO, ENV_ALPHA, ZERO },
      { TEXEL0_ALPHA, ZERO, PRIMITIVE_ALPHA, ZERO } };
   int i, v;

   memset(&in, 0, sizeof(in));
   in.prog = &prog;
   for (i = 0; i < 7; i++)
      for (v = 0; v < 256; v += 5)
      {
         uint32_t fast, ref;
         m = mux_of(TEXEL0, ZERO, SHADE, ZERO, asel[i][0], asel[i][1], asel[i][2], asel[i][3]);
         soft_combine_compile(&m, 0, &prog);
         CHECK(prog.mode == SC_TEX_MUL_SHADE && (prog.amode != SA_SAME) == (i != 4));
         in.tex0[3] = (uint8_t)v; in.shade[3] = (uint8_t)(255 - v);
         in.env[3] = (uint8_t)(v * 7); in.prim[3] = (uint8_t)(v * 3);
         fast = soft_combine_eval(&in);
         prog.mode = SC_GENERIC;
         ref = soft_combine_generic(&in);
         /* Products round like the fast colour path: within one step of generic. */
         CHECK((int)(fast >> 24) - (int)(ref >> 24) <= 1 && (int)(ref >> 24) - (int)(fast >> 24) <= 1);
      }
}

static void test_unknown_mode_falls_back(void)
{
   DecodedMux m = mux_of(NOISE, ZERO, SHADE, ZERO, ZERO, ZERO, ZERO, SHADE_ALPHA);
   m.combine.mux = 0x1234567890ull;
   soft_combine_set_logger(count_log);
   log_lines = 0;
   CHECK(soft_combine_classify(&m) == SC_TEX_MUL_SHADE);
   CHECK(soft_combine_classify(&m) == SC_TEX_MUL_SHADE);
   CHECK(log_lines == 1);
}

static void test_generic_matches_formula(void)
{
   DecodedMux m = mux_of(PRIMITIVE, ENVIRONMENT, SHADE, TEXEL0, PRIMITIVE, ENVIRONMENT, SHADE, TEXEL0);
   SoftCombineProgram prog;
   SoftCombineInputs in;
   uint32_t out;
   int r;

   soft_combine_compile(&m, 0, &prog);
   memset(&in, 0, sizeof(in));
   in.prog = &prog;
   in.prim[0] = 200; in.env[0] = 100; in.shade[0] = 128; in.tex0[0] = 10;
   out = soft_combine_generic(&in);
   r = (int)(out & 0xff);
   CHECK(r >= 59 && r <= 61);
}

static void test_alpha_cutout(void)
{
   SoftAlphaTest at = soft_alpha_test_setup(1, 1, 1, 0.0f);
   CHECK(!soft_alpha_pass(&at, 127));
   CHECK(soft_alpha_pass(&at, 128));
}

static void test_fog_blend(void)
{
   uint32_t out = soft_fog_apply(0x000000, 0xffffff, 128);
   int r = out & 0xff, g = (out >> 8) & 0xff, b = (out >> 16) & 0xff;
   CHECK(r >= 120 && r <= 136);
   CHECK(g >= 120 && g <= 136);
   CHECK(b >= 120 && b <= 136);
}


static void set_cycle(SoftCombineProgram *p, int cy, int k, int a, int b, int c, int d)
{
   p->sel[cy][k][0] = (uint8_t)a; p->sel[cy][k][1] = (uint8_t)b;
   p->sel[cy][k][2] = (uint8_t)c; p->sel[cy][k][3] = (uint8_t)d;
}

/* Folded programs on folded shade match the generic combiner within rounding, or decline. */
static void test_fold_matches_generic(void)
{
   static const int pool[] = { COMBINED, TEXEL0, PRIMITIVE, SHADE, ENVIRONMENT, ONE, ZERO,
      TEXEL0_ALPHA, PRIMITIVE_ALPHA, SHADE_ALPHA, ENV_ALPHA, COMBINED_ALPHA, PRIM_LOD_FRAC };
   uint32_t seed = 99;
   int n, ch, folded = 0, worst = 0;
   for (n = 0; n < 20000; n++)
   {
      SoftCombineProgram orig, fp;
      SoftCombineInputs in, fin;
      SoftCombineFold fold;
      uint32_t want, got;
      int cy, k, i;
      memset(&orig, 0, sizeof(orig));
      orig.mode = SC_GENERIC;
      orig.cycles = 1 + (n & 1);
      for (cy = 0; cy < 2; cy++)
         for (k = 0; k < 2; k++)
            for (i = 0; i < 4; i++)
            {
               seed = seed * 1664525u + 1013904223u;
               /* Bias towards ZERO so foldable shapes are common. */
               orig.sel[cy][k][i] = (uint8_t)((seed >> 28) < 6 ? ZERO
                     : pool[(seed >> 8) % (sizeof(pool) / sizeof(pool[0]))]);
            }
      memset(&in, 0, sizeof(in));
      for (ch = 0; ch < 4; ch++)
      {
         seed = seed * 1664525u + 1013904223u;
         in.tex0[ch] = (uint8_t)(seed >> 8); in.shade[ch] = (uint8_t)(seed >> 16);
         in.prim[ch] = (uint8_t)(seed >> 24); in.env[ch] = (uint8_t)(seed >> 4);
      }
      in.prim_lod = (uint8_t)(seed >> 12);
      in.prog = &orig;
      want = soft_combine_generic(&in);
      fp = orig;
      if (!soft_combine_fold(&fp, in.prim, in.env, in.prim_lod, &fold))
         continue;
      folded++;
      CHECK(fp.mode != SC_GENERIC && fp.cycles == 1);
      fin = in;
      fin.prog = &fp;
      soft_combine_fold_shade(&fold, fin.shade);
      got = soft_combine_eval(&fin);
      for (ch = 0; ch < 4; ch++)
      {
         int d = abs((int)((want >> (ch * 8)) & 0xff) - (int)((got >> (ch * 8)) & 0xff));
         if (d > worst)
            worst = d;
      }
   }
   CHECK(folded > 500);
   CHECK(worst <= 3);
   printf("fold: %d of 20000 folded, worst channel error %d\n", folded, worst);
}

/* Constant fills and texture-shade-prim products fold; a texel1 lerp does not. */
static void test_fold_common_programs(void)
{
   static const uint8_t prim[4] = { 10, 20, 30, 40 }, env[4] = { 1, 2, 3, 4 };
   SoftCombineProgram p;
   SoftCombineFold f;

   memset(&p, 0, sizeof(p));
   p.mode = SC_GENERIC; p.cycles = 1;
   set_cycle(&p, 0, 0, ZERO, ZERO, ZERO, PRIMITIVE);
   set_cycle(&p, 0, 1, ZERO, ZERO, ZERO, PRIMITIVE_ALPHA);
   CHECK(soft_combine_fold(&p, prim, env, 0, &f));
   CHECK(p.mode == SC_SHADE && p.amode == SA_SAME && !p.uses_tex);
   CHECK(f.src[0] < 0 && f.k[0] == 10 && f.k[3] == 40);

   memset(&p, 0, sizeof(p));
   p.mode = SC_GENERIC; p.cycles = 2;
   set_cycle(&p, 0, 0, TEXEL0, ZERO, SHADE, ZERO);
   set_cycle(&p, 0, 1, ZERO, ZERO, ZERO, TEXEL0);
   set_cycle(&p, 1, 0, COMBINED, ZERO, PRIMITIVE, ZERO);
   set_cycle(&p, 1, 1, ZERO, ZERO, ZERO, COMBINED);
   CHECK(soft_combine_fold(&p, prim, env, 0, &f));
   CHECK(p.mode == SC_TEX_MUL_SHADE && p.amode == SA_SAME && p.uses_tex);
   CHECK(f.src[0] == 0 && f.k[0] == 10 && f.src[3] < 0 && f.k[3] == 255);

   memset(&p, 0, sizeof(p));
   p.mode = SC_GENERIC; p.cycles = 1; p.uses_tex1 = 1;
   set_cycle(&p, 0, 0, TEXEL1, TEXEL0, ENV_ALPHA, TEXEL0);
   set_cycle(&p, 0, 1, ZERO, ZERO, ZERO, ONE);
   CHECK(!soft_combine_fold(&p, prim, env, 0, &f));
   CHECK(p.mode == SC_GENERIC);
}

int main(void)
{
   RUN(test_classify_tex_mul_shade);
   RUN(test_alpha_source_splits_from_colour);
   RUN(test_eval_alpha_modes_match_generic);
   RUN(test_unknown_mode_falls_back);
   RUN(test_generic_matches_formula);
   RUN(test_alpha_cutout);
   RUN(test_fog_blend);
   RUN(test_two_cycle_passthrough_uses_first_cycle);
   RUN(test_fold_matches_generic);
   RUN(test_fold_common_programs);
   TEST_EXIT();
}
