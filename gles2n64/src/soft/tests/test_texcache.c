#include <string.h>

#include "test.h"
#include "texcache.h"

static int decodes;

static void decode_fill(void *ctx, SoftTex *tex)
{
   int i;
   (void)ctx;
   decodes++;
   for (i = 0; i < tex->w * tex->h; i++)
   {
      tex->rgb[i] = 0x1234;
      tex->a[i] = 255;
   }
}

static SoftTexKey key(uint32_t crc, uint16_t w, uint16_t h)
{
   SoftTexKey k;
   memset(&k, 0, sizeof(k));
   k.crc = crc;
   k.address = 0x100000;
   k.w = w;
   k.h = h;
   return k;
}

static void test_cache_hit_returns_same(void)
{
   SoftTexKey k = key(0xabcd, 8, 8);
   const SoftTex *a, *b;

   soft_texcache_init(1024 * 1024);
   decodes = 0;
   a = soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   b = soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   CHECK(a != NULL);
   CHECK(a == b);
   CHECK(decodes == 1);
   CHECK(a->rgb[0] == 0x1234);
}

/* Equal CRCs from different RAM addresses are different textures. */
static void test_cache_separates_addresses(void)
{
   SoftTexKey k1 = key(0xabcd, 8, 8), k2 = key(0xabcd, 8, 8);
   const SoftTex *a, *b;

   k2.address = 0x180000;
   soft_texcache_init(1024 * 1024);
   decodes = 0;
   a = soft_texcache_lookup(&k1, 0x200000, decode_fill, NULL);
   b = soft_texcache_lookup(&k2, 0x200000, decode_fill, NULL);
   CHECK(a != b);
   CHECK(decodes == 2);
}

static void test_expand_replicates_bits_and_alpha(void)
{
   uint16_t rgb[3] = { 0xffff, 0x0000, 0x8410 };
   uint8_t a[3] = { 255, 7, 128 };
   uint32_t rgba[3];
   SoftTex tex;

   memset(&tex, 0, sizeof(tex));
   tex.rgb = rgb; tex.a = a; tex.rgba = rgba; tex.w = 3; tex.h = 1;
   soft_tex_expand(&tex);
   CHECK(rgba[0] == 0xffffffffu);
   CHECK(rgba[1] == 0x07000000u);
   /* 0x8410: r=16 -> 132, g=32 -> 130, b=16 -> 132 */
   CHECK(rgba[2] == (132u | 130u << 8 | 132u << 16 | 128u << 24));
}

static void test_cache_fills_rgba(void)
{
   SoftTexKey k = key(0x55, 4, 4);
   const SoftTex *t;

   soft_texcache_init(1024 * 1024);
   t = soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   CHECK(t->rgba[15] == (0x10u | 0x45u << 8 | 0xa5u << 16 | 255u << 24));
}

static void test_cache_evicts_under_budget(void)
{
   uint32_t i;

   soft_texcache_init(64 * 1024);
   for (i = 0; i < 40; i++)
   {
      SoftTexKey k = key(i + 1, 64, 32);
      CHECK(soft_texcache_lookup(&k, 0x200000, decode_fill, NULL) != NULL);
      soft_texcache_unpin_all();
      CHECK(soft_texcache_bytes() <= 65536);
   }
}

static void test_pinned_entries_survive_eviction(void)
{
   SoftTexKey a = key(1000, 64, 32);
   const SoftTex *ta;
   uint32_t i;
   int before;

   soft_texcache_init(64 * 1024);
   ta = soft_texcache_lookup(&a, 0x200000, decode_fill, NULL);
   for (i = 0; i < 40; i++)
   {
      SoftTexKey k = key(2000 + i, 64, 32);
      soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   }
   before = decodes;
   CHECK(soft_texcache_lookup(&a, 0x200000, decode_fill, NULL) == ta);
   CHECK(decodes == before);
   soft_texcache_unpin_all();
   for (i = 0; i < 40; i++)
   {
      SoftTexKey k = key(3000 + i, 64, 32);
      soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
      soft_texcache_unpin_all();
   }
   CHECK(soft_texcache_bytes() <= 65536);
}

static void test_framebuffer_texture_uses_dummy(void)
{
   SoftTexKey k = key(0x77, 16, 16);
   const SoftTex *t;

   soft_texcache_init(1024 * 1024);
   decodes = 0;
   k.address = 0x300000;
   t = soft_texcache_lookup(&k, 0x300000, decode_fill, NULL);
   CHECK(t != NULL);
   CHECK(t->w == 1 && t->h == 1);
   CHECK(decodes == 0);
}

static void test_pin_keeps_reused_texture(void)
{
   SoftTexKey a = key(5000, 64, 32);
   const SoftTex *ta;
   uint32_t i;
   int before;

   soft_texcache_init(64 * 1024);
   ta = soft_texcache_lookup(&a, 0x200000, decode_fill, NULL);
   soft_texcache_unpin_all();
   soft_texcache_pin(ta);
   for (i = 0; i < 40; i++)
   {
      SoftTexKey k = key(6000 + i, 64, 32);
      soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   }
   before = decodes;
   CHECK(soft_texcache_lookup(&a, 0x200000, decode_fill, NULL) == ta);
   CHECK(decodes == before);
   soft_texcache_pin(NULL);
}

static void test_over_budget_reported_while_pinned(void)
{
   uint32_t i;

   soft_texcache_init(64 * 1024);
   CHECK(!soft_texcache_over_budget());
   for (i = 0; i < 10; i++)
   {
      SoftTexKey k = key(7000 + i, 64, 32);
      soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   }
   CHECK(soft_texcache_over_budget());
   soft_texcache_unpin_all();
   {
      SoftTexKey k = key(8000, 64, 32);
      soft_texcache_lookup(&k, 0x200000, decode_fill, NULL);
   }
   CHECK(!soft_texcache_over_budget());
}

int main(void)
{
   RUN(test_cache_hit_returns_same);
   RUN(test_cache_separates_addresses);
   RUN(test_expand_replicates_bits_and_alpha);
   RUN(test_cache_fills_rgba);
   RUN(test_cache_evicts_under_budget);
   RUN(test_pinned_entries_survive_eviction);
   RUN(test_pin_keeps_reused_texture);
   RUN(test_over_budget_reported_while_pinned);
   RUN(test_framebuffer_texture_uses_dummy);
   TEST_EXIT();
}
