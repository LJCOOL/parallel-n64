#include <stdlib.h>
#include <string.h>

#include "texcache.h"

#define SOFT_TEX_BUCKETS 256

typedef struct Entry
{
   SoftTex tex;
   SoftTexKey key;
   size_t bytes;
   struct Entry *newer, *older;   /* LRU list, newest at head */
   struct Entry *next;            /* hash chain */
} Entry;

static Entry *buckets[SOFT_TEX_BUCKETS];
static Entry *lru_head, *lru_tail;
static size_t budget, used;

static uint16_t dummy_rgb[1];
static uint8_t  dummy_a[1] = { 255 };
static const SoftTex dummy = { dummy_rgb, dummy_a, 1, 1, SOFT_CLAMP, SOFT_CLAMP, 0, 0 };

static unsigned bucket_of(const SoftTexKey *k)
{
   return (k->crc ^ (k->crc >> 8) ^ k->w ^ ((unsigned)k->h << 3)) & (SOFT_TEX_BUCKETS - 1);
}

static int same_key(const SoftTexKey *a, const SoftTexKey *b)
{
   return a->crc == b->crc && a->w == b->w && a->h == b->h
       && a->wrap_s == b->wrap_s && a->wrap_t == b->wrap_t;
}

static void lru_unlink(Entry *e)
{
   if (e->newer) e->newer->older = e->older; else lru_head = e->older;
   if (e->older) e->older->newer = e->newer; else lru_tail = e->newer;
   e->newer = e->older = NULL;
}

static void lru_push(Entry *e)
{
   e->newer = NULL;
   e->older = lru_head;
   if (lru_head) lru_head->newer = e; else lru_tail = e;
   lru_head = e;
}

static void evict(Entry *e)
{
   Entry **pp = &buckets[bucket_of(&e->key)];
   while (*pp != e)
      pp = &(*pp)->next;
   *pp = e->next;
   lru_unlink(e);
   used -= e->bytes;
   free(e);
}

void soft_texcache_init(size_t budget_bytes)
{
   while (lru_tail)
      evict(lru_tail);
   budget = budget_bytes;
   used = 0;
}

size_t soft_texcache_bytes(void)
{
   return used;
}

const SoftTex *soft_texcache_lookup(const SoftTexKey *key, uint32_t color_image_address,
      SoftTexDecodeFn decode, void *ctx)
{
   unsigned b;
   size_t texels, bytes;
   Entry *e;

   if (key->address == color_image_address || key->w == 0 || key->h == 0)
      return &dummy;

   b = bucket_of(key);
   for (e = buckets[b]; e; e = e->next)
   {
      if (same_key(&e->key, key))
      {
         lru_unlink(e);
         lru_push(e);
         return &e->tex;
      }
   }

   texels = (size_t)key->w * key->h;
   bytes  = sizeof(Entry) + texels * 3;
   while (lru_tail && used + bytes > budget)
      evict(lru_tail);

   e = (Entry*)malloc(bytes);
   if (!e)
      return &dummy;
   memset(e, 0, sizeof(Entry));
   e->key       = *key;
   e->bytes     = bytes;
   e->tex.rgb   = (uint16_t*)(e + 1);
   e->tex.a     = (uint8_t*)(e->tex.rgb + texels);
   e->tex.w     = key->w;
   e->tex.h     = key->h;
   e->tex.wrap_s = key->wrap_s;
   e->tex.wrap_t = key->wrap_t;
   e->tex.mask_s = key->w - 1;
   e->tex.mask_t = key->h - 1;
   decode(ctx, &e->tex);

   e->next = buckets[b];
   buckets[b] = e;
   lru_push(e);
   used += bytes;
   return &e->tex;
}

static int wrap_coord(int i, int n, int mode)
{
   switch (mode)
   {
      case SOFT_CLAMP:
         return i < 0 ? 0 : (i >= n ? n - 1 : i);
      case SOFT_MIRROR:
         i %= 2 * n;
         if (i < 0) i += 2 * n;
         return i >= n ? 2 * n - 1 - i : i;
      default:
         i %= n;
         return i < 0 ? i + n : i;
   }
}

uint16_t soft_tex_sample(const SoftTex *tex, int32_t s, int32_t t, uint8_t *alpha_out)
{
   int x = wrap_coord(s >> 16, tex->w, tex->wrap_s);
   int y = wrap_coord(t >> 16, tex->h, tex->wrap_t);
   int i = y * tex->w + x;
   *alpha_out = tex->a[i];
   return tex->rgb[i];
}
