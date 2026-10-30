#include <string.h>

#include "../OpenGL.h"
#include "../gDP.h"
#include "../gSP.h"
#include "../GBI.h"
#include "../RSP.h"
#include "../gles2N64.h"
#include "../ShaderCombiner.h"
#include "../../../Graphics/RDP/gDP_state.h"
#include "../../../Graphics/RSP/gSP_state.h"
#include "soft_backend.h"
#include "soft_ogl.h"
#include "soft_xform.h"
#include "spans.h"
#include "texcache.h"
#include "soft_textures.h"
#include "../../../libretro/libretro_private.h"

static uint16_t soft_color[SOFT_WIDTH * SOFT_HEIGHT];
static uint16_t soft_depth[SOFT_WIDTH * SOFT_HEIGHT];
static uint32_t soft_front[SOFT_WIDTH * SOFT_HEIGHT];
static SoftTarget soft_target;

#define SOFT_TEXCACHE_BUDGET (3 * 1024 * 1024)

void soft_ogl_init(void)
{
   memset(soft_color, 0, sizeof(soft_color));
   memset(soft_front, 0, sizeof(soft_front));
   memset(soft_depth, 0xff, sizeof(soft_depth));
   soft_target_init(&soft_target, soft_color, soft_depth, SOFT_WIDTH, SOFT_HEIGHT);
   soft_texcache_init(SOFT_TEXCACHE_BUDGET);
}

const uint32_t *soft_frame(void)
{
   return soft_front;
}

/* The core declares XRGB8888 before the renderer is chosen, so present widens RGB565. */
void soft_present(void)
{
   static bool logged;
   int i;

   for (i = 0; i < SOFT_WIDTH * SOFT_HEIGHT; i++)
   {
      uint32_t c = soft_color[i];
      uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
      soft_front[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
   }
   if (!logged && log_cb)
      log_cb(RETRO_LOG_INFO, "soft: first frame presented\n");
   logged = true;
}

/* Float texels to 16.16, saturating instead of overflowing. */
static int32_t soft_fixed16(float v)
{
   v *= 65536.0f;
   if (v > 2147418112.0f) return 2147418112;
   if (v < -2147418112.0f) return -2147418112;
   return (int32_t)v;
}

static int soft_shift(void)
{
   return soft_scale_shift(gDP.colorImage.width);
}

static void soft_update_scissor(int shift)
{
   soft_set_scissor(&soft_target,
         (int)gDP.scissor.ulx >> shift, (int)gDP.scissor.uly >> shift,
         (int)gDP.scissor.lrx >> shift, (int)gDP.scissor.lry >> shift);
}

static void soft_viewport(SoftViewport *vp)
{
   vp->sx = gSP.viewport.vscale[0];
   vp->sy = -gSP.viewport.vscale[1];
   vp->tx = gSP.viewport.vtrans[0];
   vp->ty = gSP.viewport.vtrans[1];
   vp->sz = 32767.5f;
   vp->tz = 32767.5f;
}

/* Raster culling names screen winding; N64 back faces wind clockwise on screen. */
static int soft_cull_mode(void)
{
   if ((gSP.geometryMode & G_CULL_BOTH) == 0)
      return SOFT_CULL_NONE;
   return (gSP.geometryMode & G_CULL_BACK) ? SOFT_CULL_BACK : SOFT_CULL_FRONT;
}

static void soft_flat_colors(struct SPVertex *v, SoftVert *out)
{
   out->r = (uint8_t)(v->flat_r * 255.0f);
   out->g = (uint8_t)(v->flat_g * 255.0f);
   out->b = (uint8_t)(v->flat_b * 255.0f);
   out->a = (uint8_t)(v->flat_a * 255.0f);
}

static bool soft_mux_uses(int a, int b)
{
   int i;
   for (i = 0; i < 4; i++)
   {
      const struct CombineCycle *c = &soft_dmux.decode[i];
      if (c->sa == a || c->sb == a || c->m == a || c->a == a
            || c->sa == b || c->sb == b || c->m == b || c->a == b)
         return true;
   }
   return false;
}

static void soft_update_combiner(void)
{
   if (gDP.otherMode.cycleType == G_CYC_COPY)
      Combiner_Set(EncodeCombineMode(0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0), -1);
   else if (gDP.otherMode.cycleType == G_CYC_FILL)
      Combiner_Set(EncodeCombineMode(0, 0, 0, SHADE, 0, 0, 0, 1, 0, 0, 0, SHADE, 0, 0, 0, 1), -1);
   else
      Combiner_Set(gDP.combine.mux, -1);
}

static const SoftTex *soft_current_tex;

/* Re-decodes tile 0 only when the texture, tile or TMEM state changed. */
static const SoftTex *soft_texture(void)
{
   if ((gSP.changed & CHANGED_TEXTURE) || (gDP.changed & (CHANGED_TILE | CHANGED_TMEM))
         || soft_current_tex == NULL)
   {
      soft_current_tex = soft_texcache_get(0);
      gSP.changed &= ~CHANGED_TEXTURE;
      gDP.changed &= ~(CHANGED_TILE | CHANGED_TMEM);
   }
   return soft_current_tex;
}

/* Chooses span and per-draw state from RDP other modes and the combiner. */
static void soft_build_pipe(SoftPipe *p, bool *textured)
{
   bool zbuf, shade;

   soft_update_combiner();
   memset(p, 0, sizeof(*p));
   p->cull = soft_cull_mode();

   zbuf = ((gSP.geometryMode & G_ZBUFFER) || gDP.otherMode.depthSource == G_ZS_PRIM)
       && gDP.otherMode.cycleType <= G_CYC_2CYCLE && gDP.otherMode.depthCompare;
   p->z_test   = zbuf;
   p->z_update = zbuf && gDP.otherMode.depthUpdate;
   p->decal    = gDP.otherMode.depthMode == ZMODE_DEC;

   if (soft_combine_flags & SC_ALPHAENABLED)
   {
      p->alpha_test = 1;
      p->alpha_ref  = gDP.otherMode.cvgXAlpha ? 128 : (uint8_t)(gDP.blendColor.a * 255.0f);
      if ((soft_combine_flags & SC_ALPHAGREATER) && p->alpha_ref < 255)
         p->alpha_ref++;
   }

   *textured = gDP.otherMode.cycleType != G_CYC_FILL
            && soft_mux_uses(TEXEL0, TEXEL0_ALPHA);
   if (*textured)
   {
      p->tex = soft_texture();
      if (p->tex == NULL)
         *textured = false;
   }
   shade = soft_mux_uses(SHADE, SHADE_ALPHA);

   if (*textured)
      p->span = shade ? soft_span_tex_gouraud_z : soft_span_tex_z;
   else
      p->span = zbuf ? soft_span_gouraud_z : soft_span_gouraud;
}

static void soft_draw_list(const uint8_t *elements, uint32_t count)
{
   SoftViewport vp;
   SoftPipe pipe;
   SoftTexCoords tc;
   uint32_t i;
   bool textured;
   int shift = soft_shift();
   bool flat = (gSP.geometryMode & G_SHADE) == 0
            || (gSP.geometryMode & G_SHADING_SMOOTH) == 0;

   soft_viewport(&vp);
   soft_update_scissor(shift);
   soft_build_pipe(&pipe, &textured);
   if (textured)
      soft_tex_coords(0, &tc);

   for (i = 0; i + 2 < count; i += 3)
   {
      SoftVert sv[3];
      int k;
      for (k = 0; k < 3; k++)
      {
         struct SPVertex *v = &OGL.triangles.vertices[elements ? elements[i + k] : i + k];
         if (!soft_vertex_to_screen(v, &vp, shift, &sv[k]))
            break;
         if (flat)
            soft_flat_colors(v, &sv[k]);
         if (textured)
         {
            sv[k].s = soft_fixed16(v->s * tc.scale_s + tc.off_s);
            sv[k].t = soft_fixed16(v->t * tc.scale_t + tc.off_t);
         }
      }
      if (k == 3)
         soft_draw_tri(&soft_target, &sv[0], &sv[1], &sv[2], &pipe);
   }
}

void OGL_AddTriangle(int v0, int v1, int v2)
{
   uint32_t i;
   struct SPVertex *vtx;

   OGL.triangles.elements[OGL.triangles.num++] = v0;
   OGL.triangles.elements[OGL.triangles.num++] = v1;
   OGL.triangles.elements[OGL.triangles.num++] = v2;

   if ((gSP.geometryMode & G_SHADE) == 0)
   {
      for (i = OGL.triangles.num - 3; i < OGL.triangles.num; ++i)
      {
         vtx = &OGL.triangles.vertices[OGL.triangles.elements[i]];
         vtx->flat_r = gDP.primColor.r;
         vtx->flat_g = gDP.primColor.g;
         vtx->flat_b = gDP.primColor.b;
         vtx->flat_a = gDP.primColor.a;
      }
   }
   else if ((gSP.geometryMode & G_SHADING_SMOOTH) == 0)
   {
      struct SPVertex *vtx0 = &OGL.triangles.vertices[v0];
      for (i = OGL.triangles.num - 3; i < OGL.triangles.num; ++i)
      {
         vtx = &OGL.triangles.vertices[OGL.triangles.elements[i]];
         vtx->flat_r = vtx0->r;
         vtx->flat_g = vtx0->g;
         vtx->flat_b = vtx0->b;
         vtx->flat_a = vtx0->a;
      }
   }

   if (gDP.otherMode.depthSource == G_ZS_PRIM)
   {
      for (i = OGL.triangles.num - 3; i < OGL.triangles.num; ++i)
      {
         vtx = &OGL.triangles.vertices[OGL.triangles.elements[i]];
         vtx->z = gDP.primDepth.z * vtx->w;
      }
   }
}

void OGL_DrawTriangles(void)
{
   if (OGL.triangles.num == 0)
      return;
   soft_draw_list(OGL.triangles.elements, OGL.triangles.num);
   OGL.triangles.num = 0;
}

void OGL_DrawDMATriangles(uint32_t _numVtx)
{
   soft_draw_list(NULL, _numVtx);
}

void OGL_DrawLLETriangle(uint32_t _numVtx)
{
   OGL.triangles.num = 0;
}

static uint16_t soft_pack565(const float *c)
{
   int r = (int)(c[0] * 31.0f + 0.5f);
   int g = (int)(c[1] * 63.0f + 0.5f);
   int b = (int)(c[2] * 31.0f + 0.5f);
   return (uint16_t)((r << 11) | (g << 5) | b);
}

static void soft_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
   int x, y;
   if (x0 < soft_target.sx0) x0 = soft_target.sx0;
   if (y0 < soft_target.sy0) y0 = soft_target.sy0;
   if (x1 > soft_target.sx1) x1 = soft_target.sx1;
   if (y1 > soft_target.sy1) y1 = soft_target.sy1;
   for (y = y0; y < y1; y++)
   {
      uint16_t *row = soft_color + y * SOFT_WIDTH;
      for (x = x0; x < x1; x++)
         row[x] = c;
   }
}

void OGL_DrawRect(int ulx, int uly, int lrx, int lry, float *color)
{
   int shift = soft_shift();
   float prim[4];
   const float *c = color;

   if (gDP.otherMode.cycleType != G_CYC_FILL)
   {
      prim[0] = gDP.primColor.r;
      prim[1] = gDP.primColor.g;
      prim[2] = gDP.primColor.b;
      prim[3] = gDP.primColor.a;
      c = prim;
   }
   soft_update_scissor(shift);
   soft_fill(ulx >> shift, uly >> shift, lrx >> shift, lry >> shift, soft_pack565(c));
}

static void soft_rect_vert(SoftVert *v, float x, float y, int shift, uint16_t z, float s, float t)
{
   memset(v, 0, sizeof(*v));
   v->x = (int32_t)(x * 16.0f) >> shift;
   v->y = (int32_t)(y * 16.0f) >> shift;
   v->z = z;
   v->r = v->g = v->b = v->a = 255;
   v->s = soft_fixed16(s);
   v->t = soft_fixed16(t);
   v->invw = 1.0f;
}

void OGL_DrawTexturedRect(const struct TexturedRectParams *_params)
{
   SoftPipe pipe;
   SoftTexCoords tc;
   SoftVert v[4];
   bool textured;
   float s0, t0, s1, t1, zf;
   uint16_t z;
   int shift = soft_shift();

   soft_update_scissor(shift);
   soft_build_pipe(&pipe, &textured);
   pipe.cull = SOFT_CULL_NONE;
   if (!textured)
   {
      float prim[4] = { gDP.primColor.r, gDP.primColor.g, gDP.primColor.b, gDP.primColor.a };
      soft_fill((int)_params->ulx >> shift, (int)_params->uly >> shift,
            (int)_params->lrx >> shift, (int)_params->lry >> shift, soft_pack565(prim));
      return;
   }
   pipe.span = soft_span_tex_z;

   soft_tex_coords(0, &tc);
   s0 = _params->uls * tc.shift_s - gSP.textureTile[0]->fuls;
   t0 = _params->ult * tc.shift_t - gSP.textureTile[0]->fult;
   s1 = (_params->lrs + 1.0f) * tc.shift_s - gSP.textureTile[0]->fuls;
   t1 = (_params->lrt + 1.0f) * tc.shift_t - gSP.textureTile[0]->fult;

   zf = (gDP.otherMode.depthSource == G_ZS_PRIM) ? gDP.primDepth.z : gSP.viewport.nearz;
   zf = zf * 32767.5f + 32767.5f;
   z  = (uint16_t)(zf < 0.0f ? 0.0f : (zf > 65535.0f ? 65535.0f : zf));

   soft_rect_vert(&v[0], _params->ulx, _params->uly, shift, z, s0, t0);
   soft_rect_vert(&v[3], _params->lrx, _params->lry, shift, z, s1, t1);
   if (_params->flip)
   {
      soft_rect_vert(&v[1], _params->lrx, _params->uly, shift, z, s0, t1);
      soft_rect_vert(&v[2], _params->ulx, _params->lry, shift, z, s1, t0);
   }
   else
   {
      soft_rect_vert(&v[1], _params->lrx, _params->uly, shift, z, s1, t0);
      soft_rect_vert(&v[2], _params->ulx, _params->lry, shift, z, s0, t1);
   }
   soft_draw_tri(&soft_target, &v[0], &v[1], &v[3], &pipe);
   soft_draw_tri(&soft_target, &v[0], &v[3], &v[2], &pipe);
}

void OGL_ClearDepthBuffer(bool _fullsize)
{
   memset(soft_depth, 0xff, sizeof(soft_depth));
}

void OGL_ClearColorBuffer(float *color)
{
   uint16_t c = soft_pack565(color);
   int i;
   for (i = 0; i < SOFT_WIDTH * SOFT_HEIGHT; i++)
      soft_color[i] = c;
}

void OGL_SwapBuffers(void)
{
   int retro_return(bool a);

   if (renderCallback)
      (*renderCallback)();
   soft_present();
   retro_return(true);

   scProgramChanged = 0;
   gDP.otherMode.l = 0;
   gln64gDPSetTextureLUT(G_TT_NONE);
   ++__RSP.DList;
}
