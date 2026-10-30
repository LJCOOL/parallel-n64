#ifndef SOFT_TEXCACHE_H
#define SOFT_TEXCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_WRAP   0
#define SOFT_MIRROR 1
#define SOFT_CLAMP  2

/* Decoded texture: RGB565 and 8-bit alpha planes, w * h texels each. */
typedef struct SoftTex
{
   uint16_t *rgb;
   uint8_t *a;
   uint16_t w, h;
   uint8_t wrap_s, wrap_t;
   uint16_t mask_s, mask_t;   /* w - 1 and h - 1 */
} SoftTex;

/* Identity of a decoded texture; crc covers TMEM contents and tile parameters. */
typedef struct
{
   uint32_t crc;
   uint32_t address;
   uint16_t w, h;
   uint8_t wrap_s, wrap_t;
} SoftTexKey;

/* Fills tex->rgb and tex->a, already sized to tex->w * tex->h. */
typedef void (*SoftTexDecodeFn)(void *ctx, SoftTex *tex);

/* Drops every entry and sets the byte budget for decoded texels. */
void soft_texcache_init(size_t budget_bytes);
size_t soft_texcache_bytes(void);

/* Returns a cached texture, decoding on a miss and evicting least recently used entries.
 * A texture read from the colour image returns a 1x1 black dummy. */
const SoftTex *soft_texcache_lookup(const SoftTexKey *key, uint32_t color_image_address,
      SoftTexDecodeFn decode, void *ctx);

/* Looks up the texture for tile 0 or 1 from current RDP state. */
const SoftTex *soft_texcache_get(uint32_t tile);

/* Point-samples at 16.16 texel coordinates, applying the wrap modes. */
uint16_t soft_tex_sample(const SoftTex *tex, int32_t s, int32_t t, uint8_t *alpha_out);

/* Floors a texel coordinate, clamped to +-2^30 so any finite value converts. */
int soft_texel_index(float v);

/* Point-samples at integer texel coordinates, applying the wrap modes. */
uint16_t soft_tex_sample_i(const SoftTex *tex, int s, int t, uint8_t *alpha_out);

/* Point-samples at float texel coordinates of any magnitude. */
uint16_t soft_tex_sample_f(const SoftTex *tex, float s, float t, uint8_t *alpha_out);

#ifdef __cplusplus
}
#endif

#endif
