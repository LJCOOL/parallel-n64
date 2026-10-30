#include <string.h>

#include "test.h"
#include "raster.h"
#include "spans.h"
#include "texcache.h"

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

int main(void)
{
   RUN(test_depth_rejects_farther);
   RUN(test_decal_wins_coplanar);
   RUN(test_gouraud_midpoint);
   RUN(test_wrap_mirror_clamp);
   TEST_EXIT();
}
