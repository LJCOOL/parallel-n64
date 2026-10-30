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
#include "../../../libretro/libretro_private.h"

static uint16_t soft_color[SOFT_WIDTH * SOFT_HEIGHT];
static uint16_t soft_depth[SOFT_WIDTH * SOFT_HEIGHT];
static uint32_t soft_front[SOFT_WIDTH * SOFT_HEIGHT];
static SoftTarget soft_target;

void soft_ogl_init(void)
{
   memset(soft_color, 0, sizeof(soft_color));
   memset(soft_front, 0, sizeof(soft_front));
   memset(soft_depth, 0xff, sizeof(soft_depth));
   soft_target_init(&soft_target, soft_color, soft_depth, SOFT_WIDTH, SOFT_HEIGHT);
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

static void soft_draw_list(const uint8_t *elements, uint32_t count)
{
   SoftViewport vp;
   SoftPipe pipe;
   uint32_t i;
   int shift = soft_shift();
   bool flat = (gSP.geometryMode & G_SHADE) == 0
            || (gSP.geometryMode & G_SHADING_SMOOTH) == 0;

   soft_viewport(&vp);
   soft_update_scissor(shift);
   memset(&pipe, 0, sizeof(pipe));
   pipe.span = soft_span_flat;
   pipe.cull = soft_cull_mode();

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
