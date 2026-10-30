#include "soft_stats.h"
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
#include "combine.h"
#include "queue.h"
#include "soft_mt.h"
#include "../../../libretro/libretro_private.h"

static uint16_t soft_color[SOFT_WIDTH * SOFT_HEIGHT];
static uint16_t soft_depth[SOFT_WIDTH * SOFT_HEIGHT];
static uint32_t soft_front[SOFT_WIDTH * SOFT_HEIGHT];
static SoftTarget soft_target;
static SoftQueue soft_queue;

#define SOFT_TEXCACHE_BUDGET (7 * 1024 * 1024)

void soft_ogl_init(void)
{
   memset(soft_color, 0, sizeof(soft_color));
   memset(soft_front, 0, sizeof(soft_front));
   memset(soft_depth, 0xff, sizeof(soft_depth));
   soft_target_init(&soft_target, soft_color, soft_depth, SOFT_WIDTH, SOFT_HEIGHT);
   soft_queue_reset(&soft_queue);
   soft_texcache_init(SOFT_TEXCACHE_BUDGET);
   soft_mt_stop();
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: render threads %d\n", soft_mt_start() ? 2 : 1);
   else
      soft_mt_start();
}

/* Draws every recorded command on both cores, then releases their textures. */
static void soft_flush(void)
{
   if (!soft_queue.count)
      return;
   {
      SOFT_TIME_BEGIN(t0);
      soft_mt_replay(&soft_queue, &soft_target);
      SOFT_TIME_END(SOFT_T_RASTER, t0);
   }
   soft_texcache_unpin_all();
   soft_queue_reset(&soft_queue);
}

static void soft_record_tri(const SoftVert *v0, const SoftVert *v1, const SoftVert *v2,
      const SoftPipe *p)
{
   if (soft_queue_full(&soft_queue))
      soft_flush();
   soft_queue_tri(&soft_queue, &soft_target, v0, v1, v2, p);
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
   SOFT_TIME_BEGIN(t0);

   for (i = 0; i < SOFT_WIDTH * SOFT_HEIGHT; i++)
   {
      uint32_t c = soft_color[i];
      uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
      soft_front[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
   }
   SOFT_TIME_END(SOFT_T_PRESENT, t0);
   if (!logged && log_cb)
      log_cb(RETRO_LOG_INFO, "soft: first frame presented\n");
   logged = true;
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

static SoftCombineProgram soft_prog;

static uint8_t soft_u8(float v)
{
   return (uint8_t)(v <= 0.0f ? 0 : (v >= 1.0f ? 255 : v * 255.0f + 0.5f));
}

static void soft_log(const char *msg)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s", msg);
}

/* Chooses span and per-draw state from RDP other modes and the combiner. */
static void build_pipe(SoftPipe *p, bool *textured)
{
   bool zbuf;
   static bool logger_set;

   if (!logger_set)
   {
      soft_combine_set_logger(soft_log);
      logger_set = true;
   }
   soft_update_combiner();
   soft_combine_compile(&soft_dmux, (soft_combine_flags & SC_2CYCLE) != 0, &soft_prog);

   memset(p, 0, sizeof(*p));
   p->cull = soft_cull_mode();
   p->span = SOFT_SPAN_COMBINE;
   p->prog = &soft_prog;

   zbuf = ((gSP.geometryMode & G_ZBUFFER) || gDP.otherMode.depthSource == G_ZS_PRIM)
       && gDP.otherMode.cycleType <= G_CYC_2CYCLE && gDP.otherMode.depthCompare;
   p->z_test   = zbuf;
   p->z_update = zbuf && gDP.otherMode.depthUpdate;
   p->decal    = gDP.otherMode.depthMode == ZMODE_DEC;

   {
      SoftAlphaTest at = soft_alpha_test_setup((soft_combine_flags & SC_ALPHAENABLED) != 0,
            (soft_combine_flags & SC_ALPHAGREATER) != 0, gDP.otherMode.cvgXAlpha, gDP.blendColor.a);
      p->alpha_test = at.enabled;
      p->alpha_ref  = at.ref;
   }

   p->blend = soft_blend_classify(gDP.otherMode.l, gDP.otherMode.forceBlender,
         gDP.otherMode.alphaCvgSel, gDP.otherMode.cycleType);
   p->fog = (soft_combine_flags & SC_FOGENABLED) != 0;
   p->fog_color = soft_u8(gDP.fogColor.r) | (soft_u8(gDP.fogColor.g) << 8) | (soft_u8(gDP.fogColor.b) << 16);
   p->prim[0] = soft_u8(gDP.primColor.r); p->prim[1] = soft_u8(gDP.primColor.g);
   p->prim[2] = soft_u8(gDP.primColor.b); p->prim[3] = soft_u8(gDP.primColor.a);
   p->env[0]  = soft_u8(gDP.envColor.r);  p->env[1]  = soft_u8(gDP.envColor.g);
   p->env[2]  = soft_u8(gDP.envColor.b);  p->env[3]  = soft_u8(gDP.envColor.a);
   p->prim_lod = soft_u8(gDP.primColor.l);

   *textured = gDP.otherMode.cycleType != G_CYC_FILL && soft_prog.uses_tex;
   if (*textured)
   {
      p->tex = soft_texture();
      if (p->tex == NULL)
      {
         *textured = false;
         soft_prog.uses_tex = 0;
      }
   }
}

/* Vertex fog factor, as the GL vertex shader computes it. */
static uint8_t soft_vertex_fog(const struct SPVertex *v)
{
   float zw = v->z / v->w, f;
   if (zw < -1.0f)
      zw = -1.0f;
   f = zw * ((float)gSP.fog.multiplier / 256.0f) + (float)gSP.fog.offset / 255.0f;
   return soft_u8(f);
}

static void soft_build_pipe(SoftPipe *p, bool *textured)
{
   SOFT_TIME_BEGIN(t0);
   build_pipe(p, textured);
   SOFT_TIME_END(SOFT_T_PIPE, t0);
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
      struct SPVertex tri[3], poly[SOFT_CLIP_MAX];
      SoftVert sv[SOFT_CLIP_MAX];
      int k, n;

      for (k = 0; k < 3; k++)
         tri[k] = OGL.triangles.vertices[elements ? elements[i + k] : i + k];
      n = soft_clip_near(tri, poly);

      for (k = 0; k < n; k++)
      {
         struct SPVertex *v = &poly[k];
         if (!soft_vertex_to_screen(v, &vp, shift, &sv[k]))
            break;
         if (flat)
            soft_flat_colors(v, &sv[k]);
         if (pipe.fog)
            sv[k].fog = soft_vertex_fog(v);
         if (textured)
         {
            sv[k].s = v->s * tc.scale_s + tc.off_s;
            sv[k].t = v->t * tc.scale_t + tc.off_t;
         }
      }
      if (k < n)
         continue;
      for (k = 1; k + 1 < n; k++)
         soft_record_tri(&sv[0], &sv[k], &sv[k + 1], &pipe);
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
   if (soft_queue_full(&soft_queue))
      soft_flush();
   soft_queue_fill(&soft_queue, &soft_target, x0, y0, x1, y1, c);
}

static void soft_rect_vert(SoftVert *v, float x, float y, int shift, uint16_t z, float s, float t)
{
   memset(v, 0, sizeof(*v));
   v->x = (int32_t)(x * 16.0f) >> shift;
   v->y = (int32_t)(y * 16.0f) >> shift;
   v->z = z;
   v->r = v->g = v->b = v->a = 0;
   v->s = s;
   v->t = t;
   v->invw = 1.0f;
}

void OGL_DrawRect(int ulx, int uly, int lrx, int lry, float *color)
{
   int shift = soft_shift();

   soft_update_scissor(shift);
   if (gDP.otherMode.cycleType == G_CYC_FILL)
   {
      soft_fill(ulx >> shift, uly >> shift, lrx >> shift, lry >> shift, soft_pack565(color));
      return;
   }
   {
      /* Non-fill rectangles run through the combiner with zero shade, like the GL path. */
      SoftPipe pipe;
      SoftVert v[4];
      bool textured;
      float zf = (gDP.otherMode.depthSource == G_ZS_PRIM) ? gDP.primDepth.z : gSP.viewport.nearz;
      uint16_t z;

      zf = zf * 32767.5f + 32767.5f;
      z  = (uint16_t)(zf < 0.0f ? 0.0f : (zf > 65535.0f ? 65535.0f : zf));
      soft_build_pipe(&pipe, &textured);
      pipe.cull = SOFT_CULL_NONE;
      soft_prog.uses_tex = 0;
      soft_rect_vert(&v[0], (float)ulx, (float)uly, shift, z, 0.0f, 0.0f);
      soft_rect_vert(&v[1], (float)lrx, (float)uly, shift, z, 0.0f, 0.0f);
      soft_rect_vert(&v[2], (float)ulx, (float)lry, shift, z, 0.0f, 0.0f);
      soft_rect_vert(&v[3], (float)lrx, (float)lry, shift, z, 0.0f, 0.0f);
      soft_record_tri(&v[0], &v[1], &v[3], &pipe);
      soft_record_tri(&v[0], &v[3], &v[2], &pipe);
   }
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
   s0 = t0 = s1 = t1 = 0.0f;
   if (textured)
   {
      soft_tex_coords(0, &tc);
      s0 = _params->uls * tc.shift_s - gSP.textureTile[0]->fuls;
      t0 = _params->ult * tc.shift_t - gSP.textureTile[0]->fult;
      s1 = (_params->lrs + 1.0f) * tc.shift_s - gSP.textureTile[0]->fuls;
      t1 = (_params->lrt + 1.0f) * tc.shift_t - gSP.textureTile[0]->fult;
   }

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
   soft_record_tri(&v[0], &v[1], &v[3], &pipe);
   soft_record_tri(&v[0], &v[3], &v[2], &pipe);
}

void OGL_ClearDepthBuffer(bool _fullsize)
{
   if (soft_queue_full(&soft_queue))
      soft_flush();
   soft_queue_clear_depth(&soft_queue);
}

void OGL_ClearColorBuffer(float *color)
{
   if (soft_queue_full(&soft_queue))
      soft_flush();
   soft_queue_clear_color(&soft_queue, soft_pack565(color));
}

void soft_ogl_stop(void)
{
   soft_mt_stop();
   soft_queue_reset(&soft_queue);
   soft_texcache_unpin_all();
}

void OGL_SwapBuffers(void)
{
   int retro_return(bool a);

   if (renderCallback)
      (*renderCallback)();
   soft_flush();
   soft_present();
   retro_return(true);

   scProgramChanged = 0;
   gDP.otherMode.l = 0;
   gln64gDPSetTextureLUT(G_TT_NONE);
   ++__RSP.DList;
}
