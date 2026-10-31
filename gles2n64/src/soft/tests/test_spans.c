#include <string.h>

#include "test.h"
#include "raster.h"
#include "spans.h"
#include "texcache.h"
#include "span_pixel.h"

#define W 32
#define H 32

static uint16_t color[W * H];
static uint16_t depth[W * H];

static SoftVert vert(int px, int py, uint16_t z, uint8_t r, uint8_t g, uint8_t b)
{
   SoftVert v;
   memset(&v, 0, sizeof(v));
   v.x = px * 16;
   v.y = py * 16;
   v.z = z;
   v.r = r; v.g = g; v.b = b; v.a = 255;
   v.invw = 1.0f;
   return v;
}

static void setup(SoftTarget *t, SoftPipe *p, SoftSpanFn span)
{
   memset(color, 0, sizeof(color));
   memset(depth, 0xff, sizeof(depth));
   soft_target_init(t, color, depth, W, H);
   memset(p, 0, sizeof(*p));
   p->span = span;
   p->z_test = 1;
   p->z_update = 1;
}

static void quad(SoftTarget *t, SoftPipe *p, uint16_t z, uint8_t r, uint8_t g, uint8_t b)
{
   SoftVert a = vert(0, 0, z, r, g, b), c = vert(16, 0, z, r, g, b);
   SoftVert d = vert(16, 16, z, r, g, b), e = vert(0, 16, z, r, g, b);
   soft_draw_tri(t, &a, &c, &d, p);
   soft_draw_tri(t, &a, &d, &e, p);
}

static void test_depth_rejects_farther(void)
{
   SoftTarget t; SoftPipe p;
   setup(&t, &p, soft_span_gouraud_z);
   quad(&t, &p, 1000, 255, 0, 0);
   quad(&t, &p, 2000, 0, 0, 255);
   CHECK(color[5 * W + 5] == 0xF800);
   CHECK(depth[5 * W + 5] == 1000);
}

static void test_decal_wins_coplanar(void)
{
   SoftTarget t; SoftPipe p;
   setup(&t, &p, soft_span_gouraud_z);
   quad(&t, &p, 1000, 255, 0, 0);
   /* Same depth fails less-or-equal only through rounding; decal must still win. */
   p.decal = 1;
   quad(&t, &p, 1000 + SOFT_DECAL_BIAS / 2, 0, 0, 255);
   CHECK(color[5 * W + 5] == 0x001F);
}

static void test_gouraud_midpoint(void)
{
   SoftTarget t; SoftPipe p;
   /* 17 pixels wide so pixel 8 centre sits exactly halfway. */
   SoftVert a = vert(0, 0, 0, 255, 0, 0), b = vert(17, 0, 0, 0, 0, 255), c = vert(17, 17, 0, 0, 0, 255);
   uint16_t px;
   int r, g, bl;

   setup(&t, &p, soft_span_gouraud);
   soft_draw_tri(&t, &a, &b, &c, &p);
   px = color[0 * W + 8];
   r  = ((px >> 11) & 0x1f) * 255 / 31;
   g  = ((px >> 5) & 0x3f) * 255 / 63;
   bl = (px & 0x1f) * 255 / 31;
   CHECK(r >= 120 && r <= 136);
   CHECK(g <= 8);
   CHECK(bl >= 120 && bl <= 136);
}

static void test_wrap_mirror_clamp(void)
{
   uint16_t rgb[16];
   uint8_t a[16];
   SoftTex tex;
   uint8_t alpha;
   int i;

   for (i = 0; i < 16; i++)
   {
      rgb[i] = (uint16_t)(i % 4);
      a[i] = 255;
   }
   memset(&tex, 0, sizeof(tex));
   tex.rgb = rgb; tex.a = a; tex.w = 4; tex.h = 4;

   tex.wrap_s = SOFT_WRAP;
   CHECK(soft_tex_sample(&tex, 5 << 16, 0, &alpha) == 1);
   tex.wrap_s = SOFT_MIRROR;
   CHECK(soft_tex_sample(&tex, 5 << 16, 0, &alpha) == 2);
   tex.wrap_s = SOFT_CLAMP;
   CHECK(soft_tex_sample(&tex, 5 << 16, 0, &alpha) == 3);
   CHECK(alpha == 255);
}

/* Large repeating coordinates must still vary across the span, not saturate. */
static void test_large_tex_coords_still_vary(void)
{
   static uint16_t rgb[16];
   static uint8_t a[16];
   SoftTex tex;
   SoftTarget t; SoftPipe p;
   SoftVert v0 = vert(0, 0, 0, 255, 255, 255), v1 = vert(16, 0, 0, 255, 255, 255), v2 = vert(16, 16, 0, 255, 255, 255);
   int i, distinct = 0;
   uint16_t first;

   for (i = 0; i < 16; i++) { rgb[i] = (uint16_t)(0x0800 * (i % 4 + 1)); a[i] = 255; }
   memset(&tex, 0, sizeof(tex));
   tex.rgb = rgb; tex.a = a; tex.w = 4; tex.h = 4;
   tex.mask_s = 3; tex.mask_t = 3;

   setup(&t, &p, soft_span_tex_z);
   p.z_test = 0;
   p.tex = &tex;
   v0.s = 40000.0f; v1.s = 40016.0f; v2.s = 40016.0f;
   soft_draw_tri(&t, &v0, &v1, &v2, &p);
   first = color[0 * W + 1];
   for (i = 2; i < 15; i++)
      if (color[0 * W + i] != first)
         distinct++;
   CHECK(distinct > 0);
}

/* 4x2 RGBA texture: red rises along s, green marks the second row. */
static uint32_t filt_rgba[8];
static uint16_t filt_rgb[8];
static uint8_t filt_a[8];

static void filter_tex(SoftTex *tex)
{
   static const uint8_t red[4] = { 0, 248, 64, 128 };
   int i;
   for (i = 0; i < 8; i++)
   {
      filt_rgba[i] = red[i & 3] | (i >= 4 ? 200u << 8 : 0) | 0xffu << 24;
      filt_rgb[i] = 0;
      filt_a[i] = 255;
   }
   memset(tex, 0, sizeof(*tex));
   tex->rgba = filt_rgba; tex->rgb = filt_rgb; tex->a = filt_a;
   tex->w = 4; tex->h = 2; tex->mask_s = 3; tex->mask_t = 1;
}

static void test_three_point_filter_weights(void)
{
   SoftTex tex;
   filter_tex(&tex);
   /* Whole texel coordinates return that texel. */
   CHECK(soft_tex_filter3(&tex, 1 << 16, 0) == filt_rgba[1]);
   /* Halfway along s in the lower triangle: (0 * 16 + 248 * 16 + 16) >> 5. */
   CHECK((soft_tex_filter3(&tex, 1 << 15, 0) & 0xff) == 124);
   /* Upper triangle at (0.75, 0.75): T11 16/32, T01 8/32, T10 8/32. */
   CHECK((soft_tex_filter3(&tex, 3 << 14, 3 << 14) & 0xff) == (248 * 16 + 0 * 8 + 248 * 8 + 16) >> 5);
   CHECK(((soft_tex_filter3(&tex, 3 << 14, 3 << 14) >> 8) & 0xff) == (200 * 16 + 200 * 8 + 0 * 8 + 16) >> 5);
   /* Wrapping: s = 3.5 blends the last texel with the first. */
   CHECK((soft_tex_filter3(&tex, 7 << 15, 0) & 0xff) == (128 * 16 + 0 * 16 + 16) >> 5);
   CHECK(soft_tex_filter3(&tex, 1 << 15, 0) >> 24 == 255);
}

static int distinct_in_row(int y)
{
   int i, n = 1;
   for (i = 1; i < 16; i++)
      if (color[y * W + i] != color[y * W + i - 1])
         n++;
   return n;
}

static void draw_filter_quad(int filter)
{
   static SoftCombineProgram prog;
   SoftTex tex;
   SoftTarget t; SoftPipe p;
   SoftVert a = vert(0, 0, 0, 255, 255, 255), c = vert(16, 0, 0, 255, 255, 255);
   SoftVert d = vert(16, 16, 0, 255, 255, 255), e = vert(0, 16, 0, 255, 255, 255);

   filter_tex(&tex);
   tex.wrap_s = tex.wrap_t = SOFT_CLAMP;
   memset(&prog, 0, sizeof(prog));
   prog.mode = SC_TEX; prog.cycles = 1; prog.uses_tex = 1;
   setup(&t, &p, soft_span_fixed);
   p.z_test = 0;
   p.tex = &tex;
   p.prog = &prog;
   p.filter = filter;
   /* Two texels stretched over 16 pixels. */
   c.s = d.s = 2.0f;
   soft_draw_tri(&t, &a, &c, &d, &p);
   soft_draw_tri(&t, &a, &d, &e, &p);
}

static void test_filtered_span_blends_texels(void)
{
   int point, filtered;
   draw_filter_quad(0);
   point = distinct_in_row(0);
   draw_filter_quad(1);
   filtered = distinct_in_row(0);
   CHECK(point <= 2);
   CHECK(filtered > 4);
}

int main(void)
{
   RUN(test_three_point_filter_weights);
   RUN(test_filtered_span_blends_texels);
   RUN(test_large_tex_coords_still_vary);
   RUN(test_depth_rejects_farther);
   RUN(test_decal_wins_coplanar);
   RUN(test_gouraud_midpoint);
   RUN(test_wrap_mirror_clamp);
   TEST_EXIT();
}
