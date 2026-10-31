/* Texel decoders and format table taken from Textures.c, minus the GL-only fields. */
#ifndef SOFT_TEXELS_H
#define SOFT_TEXELS_H

#include <retro_inline.h>
#include "../N64.h"
#include "../convert.h"
#include "../Textures.h"

static INLINE uint32_t GetNone( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return 0x00000000;
}

static INLINE uint32_t GetCI4IA_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	if (x & 1)
		return IA88_RGBA4444( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B & 0x0F)] );
   return IA88_RGBA4444( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B >> 4)] );
}

static INLINE uint32_t GetCI4IA_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	if (x & 1)
		return IA88_RGBA8888( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B & 0x0F)] );
   return IA88_RGBA8888( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B >> 4)] );
}

static INLINE uint32_t GetCI4RGBA_RGBA5551( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	if (x & 1)
		return RGBA5551_RGBA5551( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B & 0x0F)] );
   return RGBA5551_RGBA5551( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B >> 4)] );
}

static INLINE uint32_t GetCI4RGBA_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	if (x & 1)
		return RGBA5551_RGBA8888( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B & 0x0F)] );
   return RGBA5551_RGBA8888( *(uint16_t*)&TMEM[256 + (palette << 4) + (color4B >> 4)] );
}

/* IA31_RGBA8888 puts alpha in the low byte; the soft planes want alpha high like the other formats. */
static INLINE uint32_t soft_ia31(uint8_t color)
{
	uint32_t i = Three2Eight[color >> 1];
	uint32_t a = One2Eight[color & 0x01];
	return (a << 24) | (i << 16) | (i << 8) | i;
}

static INLINE uint32_t GetIA31_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	return soft_ia31( (x & 1) ? (color4B & 0x0F) : (color4B >> 4) );
}

static INLINE uint32_t GetIA31_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	return IA31_RGBA4444( (x & 1) ? (color4B & 0x0F) : (color4B >> 4) );
}

static INLINE uint32_t GetI4_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	return I4_RGBA8888( (x & 1) ? (color4B & 0x0F) : (color4B >> 4) );
}

static INLINE uint32_t GetI4_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint8_t color4B = ((uint8_t*)src)[(x>>1)^(i<<1)];

	return I4_RGBA4444( (x & 1) ? (color4B & 0x0F) : (color4B >> 4) );
}

static INLINE uint32_t GetCI8IA_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return IA88_RGBA4444( *(uint16_t*)&TMEM[256 + ((uint8_t*)src)[x^(i<<1)]] );
}

static INLINE uint32_t GetCI8IA_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return IA88_RGBA8888( *(uint16_t*)&TMEM[256 + ((uint8_t*)src)[x^(i<<1)]] );
}

static INLINE uint32_t GetCI8RGBA_RGBA5551( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return RGBA5551_RGBA5551( *(uint16_t*)&TMEM[256 + ((uint8_t*)src)[x^(i<<1)]] );
}

static INLINE uint32_t GetCI8RGBA_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return RGBA5551_RGBA8888( *(uint16_t*)&TMEM[256 + ((uint8_t*)src)[x^(i<<1)]] );
}

static INLINE uint32_t GetIA44_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return IA44_RGBA8888(((uint8_t*)src)[x^(i<<1)]);
}

static INLINE uint32_t GetIA44_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return IA44_RGBA4444(((uint8_t*)src)[x^(i<<1)]);
}

static INLINE uint32_t GetI8_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return I8_RGBA8888(((uint8_t*)src)[x^(i<<1)]);
}

static INLINE uint32_t GetI8_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return I8_RGBA4444(((uint8_t*)src)[x^(i<<1)]);
}

static INLINE uint32_t GetCI16IA_RGBA8888(uint64_t *src, uint16_t x, uint16_t i, uint8_t palette)
{
	const uint16_t tex = ((uint16_t*)src)[x^i];
	const uint16_t col = (*(uint16_t*)&TMEM[256 + (tex >> 8)]);
	const uint16_t c = col >> 8;
	const uint16_t a = col & 0xFF;
	return (a << 24) | (c << 16) | (c << 8) | c;
}

static INLINE uint32_t GetCI16IA_RGBA4444(uint64_t *src, uint16_t x, uint16_t i, uint8_t palette)
{
	const uint16_t tex = ((uint16_t*)src)[x^i];
	const uint16_t col = (*(uint16_t*)&TMEM[256 + (tex >> 8)]);
	const uint16_t c = col >> 12;
	const uint16_t a = col & 0x0F;
	return (a << 12) | (c << 8) | (c << 4) | c;
}

static INLINE uint32_t GetCI16RGBA_RGBA8888(uint64_t *src, uint16_t x, uint16_t i, uint8_t palette)
{
	const uint16_t tex = ((uint16_t*)src)[x^i];
	return RGBA5551_RGBA8888(*(uint16_t*)&TMEM[256 + (tex >> 8)]);
}

static INLINE uint32_t GetCI16RGBA_RGBA5551(uint64_t *src, uint16_t x, uint16_t i, uint8_t palette)
{
	const uint16_t tex = ((uint16_t*)src)[x^i];
	return RGBA5551_RGBA5551(*(uint16_t*)&TMEM[256 + (tex >> 8)]);
}

static INLINE uint32_t GetRGBA5551_RGBA8888(uint64_t *src, uint16_t x, uint16_t i, uint8_t palette)
{
	uint16_t tex = ((uint16_t*)src)[x^i];
	return RGBA5551_RGBA8888(tex);
}

static INLINE uint32_t GetRGBA5551_RGBA5551( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	uint16_t tex = ((uint16_t*)src)[x^i];
	return RGBA5551_RGBA5551(tex);
}

static INLINE uint32_t GetIA88_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return IA88_RGBA8888(((uint16_t*)src)[x^i]);
}

static INLINE uint32_t GetIA88_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return IA88_RGBA4444(((uint16_t*)src)[x^i]);
}

static INLINE uint32_t GetRGBA8888_RGBA8888( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return ((uint32_t*)src)[x^i];
}

static INLINE uint32_t GetRGBA8888_RGBA4444( uint64_t *src, uint16_t x, uint16_t i, uint8_t palette )
{
	return RGBA8888_RGBA4444(((uint32_t*)src)[x^i]);
}


struct SoftLoadParams
{
   GetTexelFunc Get32;
   uint32_t lineShift, maxTexels;
};

/* Indexed [textureLUT][size][format]. */
static const struct SoftLoadParams soft_load_params[4][4][5] =
{
   {
      {
         { GetI4_RGBA8888, 4, 8192 },
         { GetNone, 4, 8192 },
         { GetI4_RGBA8888, 4, 8192 },
         { GetIA31_RGBA8888, 4, 8192 },
         { GetI4_RGBA8888, 4, 8192 },
      },
      {
         { GetI8_RGBA8888, 3, 4096 },
         { GetNone, 0, 4096 },
         { GetI8_RGBA8888, 3, 4096 },
         { GetIA44_RGBA8888, 3, 4096 },
         { GetI8_RGBA8888, 3, 4096 },
      },
      {
         { GetRGBA5551_RGBA8888, 2, 2048 },
         { GetNone, 2, 2048 },
         { GetIA88_RGBA8888, 2, 2048 },
         { GetIA88_RGBA8888, 2, 2048 },
         { GetNone, 0, 2048 },
      },
      {
         { GetRGBA8888_RGBA8888, 2, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
      },
   },
   {
      {
         { GetCI4RGBA_RGBA8888, 4, 4096 },
         { GetNone, 4, 8192 },
         { GetCI4RGBA_RGBA8888, 4, 4096 },
         { GetCI4RGBA_RGBA8888, 4, 4096 },
         { GetCI4RGBA_RGBA8888, 4, 4096 },
      },
      {
         { GetCI8RGBA_RGBA8888, 3, 2048 },
         { GetNone, 0, 4096 },
         { GetCI8RGBA_RGBA8888, 3, 2048 },
         { GetCI8RGBA_RGBA8888, 3, 2048 },
         { GetCI8RGBA_RGBA8888, 3, 2048 },
      },
      {
         { GetRGBA5551_RGBA8888, 2, 2048 },
         { GetNone, 2, 2048 },
         { GetNone, 0, 2048 },
         { GetCI16RGBA_RGBA8888, 2, 2048 },
         { GetNone, 0, 2048 },
      },
      {
         { GetRGBA8888_RGBA8888, 2, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
      },
   },
   {
      {
         { GetCI4RGBA_RGBA8888, 4, 4096 },
         { GetNone, 4, 8192 },
         { GetCI4RGBA_RGBA8888, 4, 4096 },
         { GetCI4RGBA_RGBA8888, 4, 4096 },
         { GetCI4RGBA_RGBA8888, 4, 4096 },
      },
      {
         { GetCI8RGBA_RGBA8888, 3, 2048 },
         { GetNone, 0, 4096 },
         { GetCI8RGBA_RGBA8888, 3, 2048 },
         { GetCI8RGBA_RGBA8888, 3, 2048 },
         { GetCI8RGBA_RGBA8888, 3, 2048 },
      },
      {
         { GetRGBA5551_RGBA8888, 2, 2048 },
         { GetNone, 2, 2048 },
         { GetNone, 0, 2048 },
         { GetCI16RGBA_RGBA8888, 2, 2048 },
         { GetNone, 0, 2048 },
      },
      {
         { GetRGBA8888_RGBA8888, 2, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
      },
   },
   {
      {
         { GetCI4IA_RGBA8888, 4, 4096 },
         { GetNone, 4, 8192 },
         { GetCI4IA_RGBA8888, 4, 4096 },
         { GetCI4IA_RGBA8888, 4, 4096 },
         { GetCI4IA_RGBA8888, 4, 4096 },
      },
      {
         { GetCI8IA_RGBA8888, 3, 2048 },
         { GetNone, 0, 4096 },
         { GetCI8IA_RGBA8888, 3, 2048 },
         { GetCI8IA_RGBA8888, 3, 2048 },
         { GetCI8IA_RGBA8888, 3, 2048 },
      },
      {
         { GetCI16IA_RGBA8888, 2, 2048 },
         { GetNone, 2, 2048 },
         { GetNone, 0, 2048 },
         { GetCI16IA_RGBA8888, 2, 2048 },
         { GetNone, 0, 2048 },
      },
      {
         { GetRGBA8888_RGBA8888, 2, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
         { GetNone, 0, 1024 },
      },
   },
};

#endif
