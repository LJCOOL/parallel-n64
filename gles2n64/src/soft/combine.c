#include <stdio.h>

#include "combine.h"

#define SOFT_LOGGED_MAX 64

static SoftLogFn logger;
static uint64_t logged[SOFT_LOGGED_MAX];
static int logged_count;

void soft_combine_set_logger(SoftLogFn fn)
{
   logger = fn;
   logged_count = 0;
}

static void log_once(const char *what, uint64_t key)
{
   char msg[96];
   int i;
   for (i = 0; i < logged_count; i++)
      if (logged[i] == key)
         return;
   if (logged_count < SOFT_LOGGED_MAX)
      logged[logged_count++] = key;
   if (logger)
   {
      snprintf(msg, sizeof(msg), "soft: %s fallback mux=%016llx\n", what, (unsigned long long)key);
      logger(msg);
   }
}

/* Alpha selectors name the same sources as colour ones. */
static int norm(int s)
{
   switch (s)
   {
      case TEXEL0_ALPHA:    return TEXEL0;
      case TEXEL1_ALPHA:    return TEXEL1;
      case SHADE_ALPHA:     return SHADE;
      case PRIMITIVE_ALPHA: return PRIMITIVE;
      case ENV_ALPHA:       return ENVIRONMENT;
      case COMBINED_ALPHA:  return COMBINED;
      default:              return s;
   }
}

static int supported(int s)
{
   switch (s)
   {
      case NOISE: case K4: case K5: case LOD_FRACTION: case CENTER: case SCALE: case UNKNOWN:
         return 0;
      default:
         return 1;
   }
}

static int is_product(const struct CombineCycle *c, int x, int y)
{
   int sa = norm(c->sa), m = norm(c->m);
   return norm(c->sb) == ZERO && norm(c->a) == ZERO
       && ((sa == x && m == y) || (sa == y && m == x));
}

static int is_single(const struct CombineCycle *c, int x)
{
   return norm(c->sa) == ZERO && norm(c->sb) == ZERO && norm(c->m) == ZERO && norm(c->a) == x;
}

static int match(const struct CombineCycle *c, SoftCombineMode mode)
{
   switch (mode)
   {
      case SC_SHADE:         return is_single(c, SHADE);
      case SC_TEX:           return is_single(c, TEXEL0);
      case SC_TEX_MUL_SHADE: return is_product(c, TEXEL0, SHADE);
      case SC_TEX_MUL_PRIM:  return is_product(c, TEXEL0, PRIMITIVE);
      case SC_TEX_MUL_ENV:   return is_product(c, TEXEL0, ENVIRONMENT);
      case SC_LERP_ENV_PRIM:
         return norm(c->sa) == PRIMITIVE && norm(c->sb) == ENVIRONMENT
             && norm(c->m) == TEXEL0 && norm(c->a) == ENVIRONMENT;
      default:
         return 0;
   }
}

static SoftAlphaMode match_alpha(const struct CombineCycle *c)
{
   if (is_single(c, SHADE))                  return SA_SHADE;
   if (is_single(c, ENVIRONMENT))            return SA_ENV;
   if (is_single(c, PRIMITIVE))              return SA_PRIM;
   if (is_single(c, TEXEL0))                 return SA_TEX;
   if (is_product(c, TEXEL0, SHADE))         return SA_TEX_MUL_SHADE;
   if (is_product(c, TEXEL0, ENVIRONMENT))   return SA_TEX_MUL_ENV;
   if (is_product(c, TEXEL0, PRIMITIVE))     return SA_TEX_MUL_PRIM;
   return SA_COUNT;
}

static SoftCombineMode classify(const DecodedMux *m, int two_cycle, int log, SoftAlphaMode *amode)
{
   int i, n = two_cycle ? 4 : 2;
   SoftCombineMode mode;

   *amode = SA_SAME;
   for (i = 0; i < n; i++)
   {
      const struct CombineCycle *c = &m->decode[i];
      if (!supported(c->sa) || !supported(c->sb) || !supported(c->m) || !supported(c->a))
      {
         if (log)
            log_once("combine", m->combine.mux);
         return SC_TEX_MUL_SHADE;
      }
   }
   if (two_cycle)
      return SC_GENERIC;
   for (mode = SC_SHADE; mode < SC_GENERIC; mode++)
      if (match(&m->decode[0], mode) && match(&m->decode[1], mode))
         return mode;
   /* Fast colour with a different simple alpha source. */
   for (mode = SC_SHADE; mode <= SC_TEX_MUL_ENV; mode++)
      if (match(&m->decode[0], mode))
      {
         SoftAlphaMode a = match_alpha(&m->decode[1]);
         if (a != SA_COUNT)
         {
            *amode = a;
            return mode;
         }
      }
   return SC_GENERIC;
}

SoftCombineMode soft_combine_classify(const DecodedMux *m)
{
   SoftAlphaMode amode;
   return classify(m, 0, 1, &amode);
}

static int uses(const SoftCombineProgram *p, int a, int b)
{
   int cy, k, i;
   for (cy = 0; cy < p->cycles; cy++)
      for (k = 0; k < 2; k++)
         for (i = 0; i < 4; i++)
            if (norm(p->sel[cy][k][i]) == a || norm(p->sel[cy][k][i]) == b)
               return 1;
   return 0;
}

/* Logs raw selectors once per mux when a one-cycle program lands on the generic path. */
static void log_generic(const DecodedMux *m, int two_cycle_in)
{
   char msg[160];
   int i;
   uint64_t key = m->combine.mux ^ 0x8000000000000000ull;
   for (i = 0; i < logged_count; i++)
      if (logged[i] == key)
         return;
   if (logged_count < SOFT_LOGGED_MAX)
      logged[logged_count++] = key;
   if (logger)
   {
      snprintf(msg, sizeof(msg), "soft: generic mux=%016llx 2cyc=%d c0=%d,%d,%d,%d a0=%d,%d,%d,%d\n",
            (unsigned long long)m->combine.mux, two_cycle_in,
            m->decode[0].sa, m->decode[0].sb, m->decode[0].m, m->decode[0].a,
            m->decode[1].sa, m->decode[1].sb, m->decode[1].m, m->decode[1].a);
      logger(msg);
   }
}

void soft_combine_compile(const DecodedMux *m, int two_cycle, SoftCombineProgram *out)
{
   int cy, k, two_cycle_in = two_cycle;
   /* A second cycle that only passes COMBINED through adds nothing. */
   if (two_cycle && is_single(&m->decode[2], COMBINED) && is_single(&m->decode[3], COMBINED))
      two_cycle = 0;
   out->cycles = two_cycle ? 2 : 1;
   for (cy = 0; cy < 2; cy++)
      for (k = 0; k < 2; k++)
      {
         const struct CombineCycle *c = &m->decode[cy * 2 + k];
         out->sel[cy][k][0] = (uint8_t)c->sa;
         out->sel[cy][k][1] = (uint8_t)c->sb;
         out->sel[cy][k][2] = (uint8_t)c->m;
         out->sel[cy][k][3] = (uint8_t)c->a;
      }
   out->mode = classify(m, two_cycle, 1, &out->amode);
   if (out->mode == SC_TEX_MUL_SHADE && out->amode == SA_SAME)
   {
      /* Fallback also covers unsupported muxes, so pin the selectors. */
      out->cycles = 1;
      out->sel[0][0][0] = TEXEL0; out->sel[0][0][1] = ZERO; out->sel[0][0][2] = SHADE; out->sel[0][0][3] = ZERO;
      out->sel[0][1][0] = TEXEL0; out->sel[0][1][1] = ZERO; out->sel[0][1][2] = SHADE; out->sel[0][1][3] = ZERO;
   }
   if (out->mode == SC_GENERIC && out->cycles == 1)
      log_generic(m, two_cycle_in);
   out->uses_tex   = uses(out, TEXEL0, TEXEL1);
   out->uses_shade = uses(out, SHADE, SHADE);
   out->uses_tex1  = uses(out, TEXEL1, TEXEL1);
}

static int input(const SoftCombineInputs *in, const uint8_t *combined, int sel, int ch)
{
   switch (sel)
   {
      case COMBINED:        return combined[ch];
      case TEXEL0:          return in->tex0[ch];
      case TEXEL1:          return in->tex1[ch];
      case PRIMITIVE:       return in->prim[ch];
      case SHADE:           return in->shade[ch];
      case ENVIRONMENT:     return in->env[ch];
      case COMBINED_ALPHA:  return combined[3];
      case TEXEL0_ALPHA:    return in->tex0[3];
      case TEXEL1_ALPHA:    return in->tex1[3];
      case PRIMITIVE_ALPHA: return in->prim[3];
      case SHADE_ALPHA:     return in->shade[3];
      case ENV_ALPHA:       return in->env[3];
      case PRIM_LOD_FRAC:   return in->prim_lod;
      case ONE:             return 255;
      default:              return 0;
   }
}

uint32_t soft_combine_generic(const SoftCombineInputs *in)
{
   const SoftCombineProgram *p = in->prog;
   uint8_t combined[4] = { 0, 0, 0, 0 };
   int cy, ch;

   for (cy = 0; cy < p->cycles; cy++)
   {
      uint8_t out[4];
      for (ch = 0; ch < 4; ch++)
      {
         const uint8_t *s = p->sel[cy][ch == 3];
         int a = input(in, combined, s[0], ch);
         int b = input(in, combined, s[1], ch);
         int c = input(in, combined, s[2], ch);
         int d = input(in, combined, s[3], ch);
         int v = (((a - b) * c * 257 + 32768) >> 16) + d;
         out[ch] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
      }
      combined[0] = out[0]; combined[1] = out[1]; combined[2] = out[2]; combined[3] = out[3];
   }
   return combined[0] | (combined[1] << 8) | (combined[2] << 16) | ((uint32_t)combined[3] << 24);
}

static uint32_t mul4(const uint8_t *x, const uint8_t *y)
{
   uint32_t r = (x[0] * y[0] + 255) >> 8, g = (x[1] * y[1] + 255) >> 8;
   uint32_t b = (x[2] * y[2] + 255) >> 8, a = (x[3] * y[3] + 255) >> 8;
   return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t pack4(const uint8_t *x)
{
   return x[0] | (x[1] << 8) | (x[2] << 16) | ((uint32_t)x[3] << 24);
}

static uint32_t eval_colour(const SoftCombineInputs *in)
{
   switch (in->prog->mode)
   {
      case SC_SHADE:         return pack4(in->shade);
      case SC_TEX:           return pack4(in->tex0);
      case SC_TEX_MUL_SHADE: return mul4(in->tex0, in->shade);
      case SC_TEX_MUL_PRIM:  return mul4(in->tex0, in->prim);
      case SC_TEX_MUL_ENV:   return mul4(in->tex0, in->env);
      default:               return soft_combine_generic(in);
   }
}

/* Alpha for a split program, rounding products like the fast colour path. */
static uint32_t eval_alpha(const SoftCombineInputs *in)
{
   const uint8_t *t = in->tex0;
   switch (in->prog->amode)
   {
      case SA_SHADE:         return in->shade[3];
      case SA_ENV:           return in->env[3];
      case SA_PRIM:          return in->prim[3];
      case SA_TEX:           return t[3];
      case SA_TEX_MUL_SHADE: return (t[3] * in->shade[3] + 255) >> 8;
      case SA_TEX_MUL_ENV:   return (t[3] * in->env[3] + 255) >> 8;
      default:               return (t[3] * in->prim[3] + 255) >> 8;
   }
}

uint32_t soft_combine_eval(const SoftCombineInputs *in)
{
   uint32_t c = eval_colour(in);
   if (in->prog->mode == SC_GENERIC || in->prog->amode == SA_SAME)
      return c;
   return (c & 0x00ffffffu) | eval_alpha(in) << 24;
}

SoftBlendMode soft_blend_classify(uint32_t other_mode_l, int force_blend,
      int alpha_cvg_sel, int cycle_type)
{
   uint32_t mode = other_mode_l >> 16;

   if (alpha_cvg_sel && (other_mode_l & 0x7000) != 0x7000)
      return (mode == 0x4055 || mode == 0x5055) ? SB_KEEP : SB_OPAQUE;
   if (!force_blend || cycle_type >= 2)
      return SB_OPAQUE;

   switch (mode)
   {
      case 0x0382: case 0x0091: case 0x0C08: case 0x0F0A: case 0x0302: case 0xA500:
      case 0xCB02: case 0xC800: case 0x07C2: case 0x00C0: case 0xC302: case 0xC702:
         return SB_OPAQUE;
      case 0x0F1A:
         return cycle_type == 0 ? SB_OPAQUE : SB_KEEP;
      case 0x0448: case 0x055A: case 0xC912:
         return SB_ADD_FOG_ALPHA;
      case 0xC712: case 0xAF50: case 0x0F5A: case 0x0FA5: case 0x5055:
         return SB_KEEP;
      case 0xC810: case 0xC811: case 0xF550:
         return SB_FOG;
      case 0x5F50: case 0x5000: case 0x0150: case 0x0550: case 0x0D18:
      case 0x0040: case 0x0C18: case 0x0C19: case 0x0050: case 0x0051: case 0x0055:
         return SB_ALPHA;
      default:
         log_once("blend", mode);
         return SB_ALPHA;
   }
}

SoftAlphaTest soft_alpha_test_setup(int enabled, int greater, int cvg_x_alpha, float blend_alpha)
{
   SoftAlphaTest t;
   int ref = cvg_x_alpha ? 128 : (int)(blend_alpha * 255.0f + 0.5f);
   t.enabled = enabled;
   if (!greater)
      ref++;
   t.ref = (uint8_t)(ref > 255 ? 255 : ref);
   return t;
}

uint32_t soft_fog_apply(uint32_t color, uint32_t fog_color, uint8_t factor)
{
   uint32_t out = color & 0xff000000;
   int ch;
   for (ch = 0; ch < 3; ch++)
   {
      int c = (color >> (ch * 8)) & 0xff, f = (fog_color >> (ch * 8)) & 0xff;
      out |= (uint32_t)(c + (((f - c) * factor + 127) / 255)) << (ch * 8);
   }
   return out;
}
