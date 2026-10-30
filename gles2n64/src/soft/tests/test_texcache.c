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

static void test_cache_evicts_under_budget(void)
{
   uint32_t i;

   soft_texcache_init(64 * 1024);
   for (i = 0; i < 40; i++)
   {
      SoftTexKey k = key(i + 1, 64, 32);
      CHECK(soft_texcache_lookup(&k, 0x200000, decode_fill, NULL) != NULL);
      CHECK(soft_texcache_bytes() <= 65536);
   }
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

int main(void)
{
   RUN(test_cache_hit_returns_same);
   RUN(test_cache_evicts_under_budget);
   RUN(test_framebuffer_texture_uses_dummy);
   TEST_EXIT();
}
