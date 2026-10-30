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

int main(void)
{
   RUN(test_classify_tex_mul_shade);
   RUN(test_unknown_mode_falls_back);
   RUN(test_generic_matches_formula);
   RUN(test_alpha_cutout);
   RUN(test_fog_blend);
   RUN(test_two_cycle_passthrough_uses_first_cycle);
   TEST_EXIT();
}
