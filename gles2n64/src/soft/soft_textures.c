#include "soft_stats.h"
#include <retro_miscellaneous.h>

#include "../Common.h"
#include "../GBI.h"
#include "../RSP.h"
#include "../gDP.h"
#include "../gSP.h"
#include "../Hash.h"
#include "../../../Graphics/RDP/gDP_state.h"
#include "../../../Graphics/RSP/gSP_state.h"
#include "soft_texels.h"
#include "soft_textures.h"
#include "texcache.h"

struct TileSizes
{
   uint32_t maskWidth, clampWidth, width, realWidth;
   uint32_t maskHeight, clampHeight, height, realHeight;
};

/* Same sizing rules as the GL texture cache. */
static void calc_tile_sizes(uint32_t _t, struct TileSizes *_sizes, struct gDPTile *_pLoadTile)
{
   uint32_t lineWidth, lineHeight, maskWidth, maskHeight, width, height;
   struct gDPLoadTileInfo *info;
   struct gDPTile *pTile = _t < 2 ? gSP.textureTile[_t] : &gDP.tiles[_t];
   const struct SoftLoadParams *loadParams = &soft_load_params[gDP.otherMode.textureLUT][pTile->size][pTile->format];
   const uint32_t maxTexels = loadParams->maxTexels;
   const uint32_t tileWidth = ((pTile->lrs - pTile->uls) & 0x03FF) + 1;
   const uint32_t tileHeight = ((pTile->lrt - pTile->ult) & 0x03FF) + 1;

   (void)_pLoadTile;
   lineWidth = pTile->line << loadParams->lineShift;
   lineHeight = lineWidth != 0 ? MIN(maxTexels / lineWidth, tileHeight) : 0;

   maskWidth = 1 << pTile->masks;
   maskHeight = 1 << pTile->maskt;

   info = &gDP.loadInfo[pTile->tmem];
   if (info->loadType == LOADTYPE_TILE)
   {
      if (pTile->masks && ((maskWidth * maskHeight) <= maxTexels))
         width = maskWidth;
      else
      {
         width = MIN(info->width, info->texWidth);
         if (info->size > pTile->size)
            width <<= info->size - pTile->size;
      }
      if (pTile->maskt && ((maskWidth * maskHeight) <= maxTexels))
         height = maskHeight;
      else
         height = info->height;
   }
   else
   {
      if (pTile->masks && ((maskWidth * maskHeight) <= maxTexels))
         width = maskWidth;
      else if ((tileWidth * tileHeight) <= maxTexels)
         width = tileWidth;
      else
         width = lineWidth;

      if (pTile->maskt && ((maskWidth * maskHeight) <= maxTexels))
         height = maskHeight;
      else if ((tileWidth * tileHeight) <= maxTexels)
         height = tileHeight;
      else
         height = lineHeight;
   }

   _sizes->clampWidth = (pTile->clamps && gDP.otherMode.cycleType != G_CYC_COPY) ? tileWidth : width;
   _sizes->clampHeight = (pTile->clampt && gDP.otherMode.cycleType != G_CYC_COPY) ? tileHeight : height;

   if (_sizes->clampWidth > 256)
      pTile->clamps = 0;
   if (_sizes->clampHeight > 256)
      pTile->clampt = 0;

   if (maskWidth > width)
   {
      pTile->masks = powof(width);
      maskWidth = 1 << pTile->masks;
   }
   if (maskHeight > height)
   {
      pTile->maskt = powof(height);
      maskHeight = 1 << pTile->maskt;
   }

   _sizes->maskWidth = maskWidth;
   _sizes->maskHeight = maskHeight;
   _sizes->width = width;
   _sizes->height = height;

   if (pTile->clamps != 0)
      _sizes->realWidth = _sizes->clampWidth;
   else if (pTile->masks != 0)
      _sizes->realWidth = _sizes->maskWidth;
   else
      _sizes->realWidth = _sizes->width;

   if (pTile->clampt != 0)
      _sizes->realHeight = _sizes->clampHeight;
   else if (pTile->maskt != 0)
      _sizes->realHeight = _sizes->maskHeight;
   else
      _sizes->realHeight = _sizes->height;
}

struct DecodeCtx
{
   struct gDPTile *tile;
   struct TileSizes sizes;
};

static uint32_t tile_crc(struct gDPTile *tile, const struct TileSizes *sz)
{
   const uint32_t lineBytes = tile->line << 3;
   uint32_t crc = 0xFFFFFFFF;
   uint32_t params[8];

   crc = Hash_Calculate(crc, &TMEM[tile->tmem], sz->height * lineBytes);
   if (tile->size == G_IM_SIZ_32b)
      crc = Hash_Calculate(crc, &TMEM[tile->tmem + 256], sz->height * lineBytes);
   if (gDP.otherMode.textureLUT != G_TT_NONE || tile->format == G_IM_FMT_CI)
   {
      if (tile->size == G_IM_SIZ_4b)
         crc = Hash_Calculate(crc, &gDP.paletteCRC16[tile->palette], 4);
      else if (tile->size == G_IM_SIZ_8b)
         crc = Hash_Calculate(crc, &gDP.paletteCRC256, 4);
   }
   params[0] = sz->width;      params[1] = sz->height;
   params[2] = sz->clampWidth; params[3] = sz->clampHeight;
   params[4] = tile->masks | (tile->maskt << 8) | (tile->mirrors << 16) | (tile->mirrort << 24);
   params[5] = tile->clamps | (tile->clampt << 8) | (tile->format << 16) | (tile->size << 24);
   params[6] = gDP.otherMode.textureLUT;
   params[7] = tile->palette;
   return Hash_Calculate(crc, params, sizeof(params));
}

static void store(SoftTex *tex, int j, uint32_t c)
{
   uint32_t r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
   tex->rgb[j] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
   tex->a[j]   = (uint8_t)(c >> 24);
}

/* Mirrors TextureCache_getTextureDestData, writing RGB565 + A8 planes. */
static void decode_tile_impl(void *opaque, SoftTex *tex)
{
   struct DecodeCtx *ctx = (struct DecodeCtx*)opaque;
   struct gDPTile *tile = ctx->tile;
   const struct TileSizes *sz = &ctx->sizes;
   uint16_t mirrorSBit, maskSMask, clampSClamp;
   uint16_t mirrorTBit, maskTMask, clampTClamp;
   uint32_t x, y, j = 0;
   uint16_t tx, ty;

   if (tile->masks > 0)
   {
      clampSClamp = tile->clamps ? sz->clampWidth - 1 : (tile->mirrors ? (sz->width << 1) - 1 : sz->width - 1);
      maskSMask = (1 << tile->masks) - 1;
      mirrorSBit = (tile->mirrors != 0 || sz->realWidth / sz->width == 2) ? 1 << tile->masks : 0;
   }
   else
   {
      clampSClamp = MIN(sz->clampWidth, sz->width) - 1;
      maskSMask = 0xFFFF;
      mirrorSBit = 0;
   }
   if (tile->maskt > 0)
   {
      clampTClamp = tile->clampt ? sz->clampHeight - 1 : (tile->mirrort ? (sz->height << 1) - 1 : sz->height - 1);
      maskTMask = (1 << tile->maskt) - 1;
      mirrorTBit = (tile->mirrort != 0 || sz->realHeight / sz->height == 2) ? 1 << tile->maskt : 0;
   }
   else
   {
      clampTClamp = MIN(sz->clampHeight, sz->height) - 1;
      maskTMask = 0xFFFF;
      mirrorTBit = 0;
   }

   if (tile->size == G_IM_SIZ_32b)
   {
      const uint16_t *tmem16 = (const uint16_t*)TMEM;
      const uint32_t tbase = tile->tmem << 2;
      int wid_64 = sz->clampWidth << 2, line32;
      if (wid_64 & 15) wid_64 += 16;
      wid_64 &= 0xFFFFFFF0;
      wid_64 >>= 3;
      line32 = tile->line << 1;
      line32 = (line32 - wid_64) << 3;
      if (wid_64 < 1) wid_64 = 1;
      line32 = (wid_64 << 1) + (line32 >> 2);

      for (y = 0; y < sz->realHeight; ++y)
      {
         uint32_t tline, xorval;
         ty = MIN(y, clampTClamp) & maskTMask;
         if (y & mirrorTBit)
            ty ^= maskTMask;
         tline = tbase + line32 * ty;
         xorval = (ty & 1) ? 3 : 1;
         for (x = 0; x < sz->realWidth; ++x)
         {
            uint32_t taddr;
            uint16_t gr, ab;
            tx = MIN(x, clampSClamp) & maskSMask;
            if (x & mirrorSBit)
               tx ^= maskSMask;
            taddr = ((tline + tx) ^ xorval) & 0x3ff;
            gr = swapword(tmem16[taddr]);
            ab = swapword(tmem16[taddr | 0x400]);
            store(tex, j++, ((uint32_t)ab << 16) | gr);
         }
      }
      return;
   }

   if (tile->format == G_IM_FMT_YUV)
   {
      for (j = 0; j < sz->realWidth * sz->realHeight; j++)
         store(tex, j, 0xff000000);
      return;
   }

   {
      GetTexelFunc GetTexel = soft_load_params[gDP.otherMode.textureLUT][tile->size][tile->format].Get32;
      uint32_t tMemMask = gDP.otherMode.textureLUT == G_TT_NONE ? 0x1FF : 0xFF;
      for (y = 0; y < sz->realHeight; ++y)
      {
         uint64_t *pSrc;
         uint16_t i;
         ty = MIN(y, clampTClamp) & maskTMask;
         if (y & mirrorTBit)
            ty ^= maskTMask;
         pSrc = &TMEM[(tile->tmem + tile->line * ty) & tMemMask];
         i = (ty & 1) << 1;
         for (x = 0; x < sz->realWidth; ++x)
         {
            tx = MIN(x, clampSClamp) & maskSMask;
            if (x & mirrorSBit)
               tx ^= maskSMask;
            store(tex, j++, GetTexel(pSrc, tx, i, tile->palette));
         }
      }
   }
}

static void decode_tile(void *opaque, SoftTex *tex)
{
   SOFT_TIME_BEGIN(t0);
   decode_tile_impl(opaque, tex);
   SOFT_TIME_END(SOFT_T_DECODE, t0);
}

const SoftTex *soft_texcache_get(uint32_t t)
{
   struct DecodeCtx ctx;
   SoftTexKey key;

   if (gSP.textureTile[t] == NULL)
      return NULL;
   if (gSP.texture.tile == 7 && t == 0 && gSP.textureTile[0] == gDP.loadTile
         && gDP.loadTile->loadType == LOADTYPE_BLOCK && gSP.textureTile[0]->tmem == gSP.textureTile[1]->tmem)
      gSP.textureTile[0] = gSP.textureTile[1];

   ctx.tile = gSP.textureTile[t];
   calc_tile_sizes(t, &ctx.sizes, gDP.loadTile);
   if (ctx.sizes.realWidth == 0 || ctx.sizes.realHeight == 0
         || ctx.sizes.realWidth > 1024 || ctx.sizes.realHeight > 1024)
      return NULL;

   {
      SOFT_TIME_BEGIN(t0);
      key.crc  = tile_crc(ctx.tile, &ctx.sizes);
      SOFT_TIME_END(SOFT_T_TEXCRC, t0);
   }
   key.address = gDP.textureImage.address;
   key.w       = (uint16_t)ctx.sizes.realWidth;
   key.h       = (uint16_t)ctx.sizes.realHeight;
   key.wrap_s  = ctx.tile->clamps ? SOFT_CLAMP : (ctx.tile->mirrors ? SOFT_MIRROR : SOFT_WRAP);
   key.wrap_t  = ctx.tile->clampt ? SOFT_CLAMP : (ctx.tile->mirrort ? SOFT_MIRROR : SOFT_WRAP);
   return soft_texcache_lookup(&key, gDP.colorImage.address, decode_tile, &ctx);
}

static float shift_scale(uint32_t shift)
{
   if (shift > 10)
      return (float)(1 << (16 - shift));
   if (shift > 0)
      return 1.0f / (float)(1 << shift);
   return 1.0f;
}

void soft_tex_coords(uint32_t t, SoftTexCoords *out)
{
   struct gDPTile *tile = gSP.textureTile[t];
   out->shift_s = shift_scale(tile->shifts);
   out->shift_t = shift_scale(tile->shiftt);
   out->scale_s = gSP.texture.scales * out->shift_s;
   out->scale_t = gSP.texture.scalet * out->shift_t;
   out->off_s   = 0.5f - tile->fuls;
   out->off_t   = 0.5f - tile->fult;
}
