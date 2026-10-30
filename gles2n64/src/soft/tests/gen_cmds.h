/* Random draw commands shared by the queue and thread tests. */
#ifndef SOFT_GEN_CMDS_H
#define SOFT_GEN_CMDS_H

#include <string.h>

#include "raster.h"
#include "spans.h"
#include "texcache.h"
#include "combine.h"
#include "queue.h"

#define GW 96
#define GH 72
#define GTW 16

static uint32_t gen_seed = 777;
static uint16_t gen_rgb[GTW * GTW];
static uint8_t gen_a[GTW * GTW];
static uint32_t gen_rgba[GTW * GTW];
static SoftTex gen_tex;

static uint32_t gen_rnd(void)
{
   gen_seed = gen_seed * 1664525u + 1013904223u;
   return gen_seed >> 8;
}

static void gen_init(void)
{
   int i;
   for (i = 0; i < GTW * GTW; i++)
   {
      gen_rgb[i] = (uint16_t)gen_rnd();
      gen_a[i] = (uint8_t)gen_rnd();
   }
   memset(&gen_tex, 0, sizeof gen_tex);
   gen_tex.rgb = gen_rgb; gen_tex.a = gen_a; gen_tex.rgba = gen_rgba;
   gen_tex.w = gen_tex.h = GTW;
   gen_tex.mask_s = gen_tex.mask_t = GTW - 1;
   soft_tex_expand(&gen_tex);
}

static SoftVert gen_vert(void)
{
   SoftVert v;
   memset(&v, 0, sizeof v);
   v.x = (int32_t)(gen_rnd() % ((GW + 40) * 16)) - 20 * 16;
   v.y = (int32_t)(gen_rnd() % ((GH + 40) * 16)) - 20 * 16;
   v.z = (uint16_t)gen_rnd();
   v.r = (uint8_t)gen_rnd(); v.g = (uint8_t)gen_rnd();
   v.b = (uint8_t)gen_rnd(); v.a = (uint8_t)gen_rnd();
   v.fog = (uint8_t)gen_rnd();
   v.invw = 0.5f + (float)(gen_rnd() & 0xff) / 256.0f;
   v.s = (float)(gen_rnd() % 64) * v.invw;
   v.t = (float)(gen_rnd() % 64) * v.invw;
   return v;
}

static void gen_pipe(SoftPipe *p, SoftCombineProgram *prog)
{
   int i;
   memset(prog, 0, sizeof *prog);
   prog->mode = (SoftCombineMode)(gen_rnd() % (SC_GENERIC + 1));
   prog->cycles = 1;
   prog->uses_tex = 1;
   prog->uses_shade = 1;
   prog->sel[0][0][0] = TEXEL0; prog->sel[0][0][1] = ENVIRONMENT;
   prog->sel[0][0][2] = SHADE;  prog->sel[0][0][3] = ENVIRONMENT;
   prog->sel[0][1][0] = TEXEL0; prog->sel[0][1][1] = ZERO;
   prog->sel[0][1][2] = SHADE;  prog->sel[0][1][3] = ZERO;
   if (prog->mode <= SC_TEX_MUL_ENV)
      prog->amode = (SoftAlphaMode)(gen_rnd() % SA_COUNT);
   memset(p, 0, sizeof *p);
   p->span = soft_span_fixed;
   p->tex = &gen_tex;
   p->prog = prog;
   p->cull = (int)(gen_rnd() % 3);
   p->z_test = (gen_rnd() & 3) != 0;
   p->z_update = gen_rnd() & 1;
   p->decal = (gen_rnd() & 7) == 0;
   p->alpha_test = gen_rnd() & 1;
   p->alpha_ref = (uint8_t)gen_rnd();
   p->fog = gen_rnd() & 1;
   p->fog_color = gen_rnd();
   p->blend = (int)(gen_rnd() % (SB_KEEP + 1));
   for (i = 0; i < 4; i++)
   {
      p->prim[i] = (uint8_t)gen_rnd();
      p->env[i] = (uint8_t)gen_rnd();
   }
}

static void gen_scissor(SoftTarget *t)
{
   int x0 = (int)(gen_rnd() % GW), y0 = (int)(gen_rnd() % GH);
   soft_set_scissor(t, x0, y0, x0 + 1 + (int)(gen_rnd() % GW), y0 + 1 + (int)(gen_rnd() % GH));
}

/* Reference fill and clears, matching the renderer's immediate behaviour. */
static void gen_fill_now(SoftTarget *t, int x0, int y0, int x1, int y1, uint16_t c)
{
   int x, y;
   if (x0 < t->sx0) x0 = t->sx0;
   if (y0 < t->sy0) y0 = t->sy0;
   if (x1 > t->sx1) x1 = t->sx1;
   if (y1 > t->sy1) y1 = t->sy1;
   for (y = y0; y < y1; y++)
      for (x = x0; x < x1; x++)
         t->color[y * t->width + x] = c;
}

/* Draws one random command into target a now and records it into q for b. */
static void gen_command(SoftTarget *a, SoftTarget *b, SoftQueue *q, SoftCombineProgram *prog)
{
   uint32_t kind = gen_rnd() % 100;
   gen_scissor(a);
   b->sx0 = a->sx0; b->sy0 = a->sy0; b->sx1 = a->sx1; b->sy1 = a->sy1;
   if (kind < 85)
   {
      SoftPipe p;
      SoftVert v0 = gen_vert(), v1 = gen_vert(), v2 = gen_vert();
      gen_pipe(&p, prog);
      soft_draw_tri(a, &v0, &v1, &v2, &p);
      soft_queue_tri(q, b, &v0, &v1, &v2, &p);
      prog->mode = SC_SHADE;  /* the queue must hold its own copy */
   }
   else if (kind < 95)
   {
      int x0 = (int)(gen_rnd() % GW) - 4, y0 = (int)(gen_rnd() % GH) - 4;
      int x1 = x0 + (int)(gen_rnd() % GW), y1 = y0 + (int)(gen_rnd() % GH);
      uint16_t c = (uint16_t)gen_rnd();
      gen_fill_now(a, x0, y0, x1, y1, c);
      soft_queue_fill(q, b, x0, y0, x1, y1, c);
   }
   else if (kind < 98)
   {
      uint16_t c = (uint16_t)gen_rnd();
      int i;
      for (i = 0; i < a->width * a->height; i++)
         a->color[i] = c;
      soft_queue_clear_color(q, c);
   }
   else
   {
      memset(a->depth, 0xff, (size_t)a->width * a->height * sizeof(uint16_t));
      soft_queue_clear_depth(q);
   }
}

#endif
