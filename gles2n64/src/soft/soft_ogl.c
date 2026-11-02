#include "soft_stats.h"
#include <string.h>

#include "../OpenGL.h"
#include "../gDP.h"
#include "../gSP.h"
#include "../GBI.h"
#include "../RSP.h"
#include "../gles2N64.h"
#include "../N64.h"
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

static int soft_tex_filter = 0;
static int soft_async = 1;

void soft_set_tex_filter(int mode)
{
   soft_tex_filter = mode;
}

void soft_set_async(int on)
{
   soft_async = on;
}

static uint16_t soft_color[SOFT_WIDTH * SOFT_HEIGHT];
static uint16_t soft_depth[SOFT_WIDTH * SOFT_HEIGHT];
/* The worker presents into one front buffer while the frontend shows the other. */
static uint32_t soft_fronts[2][SOFT_WIDTH * SOFT_HEIGHT];
static int soft_shown;
static SoftTarget soft_target;
static SoftQueue soft_queues[2];
static SoftQueue *soft_q = &soft_queues[0];
static const SoftTex *soft_current_tex[2];
static const struct gDPTile *soft_current_tile[2];

#define SOFT_TEXCACHE_BUDGET (7 * 1024 * 1024)

void soft_ogl_init(void)
{
   soft_mt_stop();
   memset(soft_color, 0, sizeof(soft_color));
   memset(soft_fronts, 0, sizeof(soft_fronts));
   memset(soft_depth, 0xff, sizeof(soft_depth));
   soft_target_init(&soft_target, soft_color, soft_depth, SOFT_WIDTH, SOFT_HEIGHT);
   soft_queue_reset(soft_q);
   soft_texcache_init(SOFT_TEXCACHE_BUDGET);
   soft_current_tex[0] = soft_current_tex[1] = NULL;
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "soft: render threads %d\n", soft_mt_start() ? 2 : 1);
   else
      soft_mt_start();
}

/* Finishes the frame drawing ahead and releases its textures. */
static void soft_wait_ahead(void)
{
   soft_mt_wait();
   soft_texcache_unhold();
}

/* Draws every recorded command on both cores, then releases their textures. */
static void soft_flush(void)
{
   soft_wait_ahead();
   if (!soft_q->count)
      return;
   {
      SOFT_TIME_BEGIN(t0);
      soft_mt_replay(soft_q, &soft_target);
      SOFT_TIME_END(SOFT_T_RASTER, t0);
   }
   soft_texcache_unpin_all();
   soft_queue_reset(soft_q);
}

static SoftCombineFold soft_fold;
static int soft_folded;

static void soft_fold_vert(const SoftVert *in, SoftVert *out)
{
   uint8_t c[4] = { in->r, in->g, in->b, in->a };
   *out = *in;
   soft_combine_fold_shade(&soft_fold, c);
   out->r = c[0]; out->g = c[1]; out->b = c[2]; out->a = c[3];
}

static void soft_record_tri(const SoftVert *v0, const SoftVert *v1, const SoftVert *v2,
      const SoftPipe *p)
{
   SoftVert f[3];
   if (soft_folded)
   {
      soft_fold_vert(v0, &f[0]); soft_fold_vert(v1, &f[1]); soft_fold_vert(v2, &f[2]);
      v0 = &f[0]; v1 = &f[1]; v2 = &f[2];
   }
   /* Flushing unpins textures, so pin this one again after any flush. */
   if (soft_queue_full(soft_q) || soft_texcache_over_budget())
      soft_flush();
   soft_texcache_pin(p->tex);
   soft_texcache_pin(p->tex1);
   {
      SOFT_TIME_BEGIN(t0);
      soft_queue_tri(soft_q, &soft_target, v0, v1, v2, p);
      SOFT_TIME_END(SOFT_T_RECORD, t0);
   }
}

const uint32_t *soft_frame(void)
{
   return soft_fronts[soft_shown];
}

/* The core declares XRGB8888 before the renderer is chosen, so present widens RGB565. */
static void soft_present_to(uint32_t *front)
{
   int i;
   SOFT_TIME_BEGIN(t0);

   for (i = 0; i < SOFT_WIDTH * SOFT_HEIGHT; i++)
   {
      uint32_t c = soft_color[i];
      uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
      front[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
   }
   SOFT_TIME_END(SOFT_T_PRESENT, t0);
}

#define SOFT_FRAME_BYTES (SOFT_WIDTH * SOFT_HEIGHT * 2)

/* RDRAM address of the frame being presented, or 0 when it cannot be written back. */
static uint32_t soft_wb_addr;
/* This frame's on-screen colour image, and whether it has draws not yet in RDRAM. */
static uint32_t soft_main_addr;
static int soft_main_dirty;
/* Offscreen colour image being drawn, its width and the rows its scissors reached. */
static uint32_t soft_off_addr;
static int soft_off_w, soft_off_rows;
static uint16_t soft_save_color[SOFT_WIDTH * SOFT_HEIGHT];
static uint16_t soft_save_depth[SOFT_WIDTH * SOFT_HEIGHT];

/* Stores rows of the colour buffer as RGBA5551 with full coverage, as the RDP would. */
static void soft_store_rdram(uint32_t addr, int w, int rows)
{
   uint16_t *dst = (uint16_t*)gfx_info.RDRAM;
   uint32_t base = addr >> 1;
   int i, n = w * rows;

   for (i = 0; i < n; i++)
   {
      uint32_t c = soft_color[i];
      dst[(base + i) ^ 1] = (uint16_t)((c & 0xffc0) | (c & 0x1f) << 1 | 1);
   }
}

static void soft_load_rdram(uint32_t addr, int w, int rows)
{
   const uint16_t *src = (const uint16_t*)gfx_info.RDRAM;
   uint32_t base = addr >> 1;
   int i, n = w * rows;

   for (i = 0; i < n; i++)
   {
      uint32_t c = src[(base + i) ^ 1];
      uint32_t g = (c >> 6) & 0x1f;
      soft_color[i] = (uint16_t)((c & 0xf800) | ((g << 1 | g >> 4) << 5) | ((c >> 1) & 0x1f));
   }
}

static void soft_write_back(void)
{
   if (soft_wb_addr)
      soft_store_rdram(soft_wb_addr, SOFT_WIDTH, SOFT_HEIGHT);
}

static int soft_frame_fits(uint32_t a)
{
   return !(a & 3) && a + SOFT_FRAME_BYTES <= RDRAMSize;
}

static void soft_capture_wb_addr(void)
{
   soft_wb_addr = soft_main_addr && soft_frame_fits(soft_main_addr) ? soft_main_addr : 0;
}

/* Finishes the offscreen image, stores it to RDRAM and restores the on-screen buffers. */
static void soft_unbind(void)
{
   soft_flush();
   soft_store_rdram(soft_off_addr, soft_off_w, soft_off_rows);
   memcpy(soft_color, soft_save_color, sizeof(soft_color));
   memcpy(soft_depth, soft_save_depth, sizeof(soft_depth));
   soft_target_init(&soft_target, soft_color, soft_depth, SOFT_WIDTH, SOFT_HEIGHT);
   soft_off_addr = 0;
}

/* Routes draws to the current colour image; one other than the frame's renders through RDRAM. */
static void soft_bind(void)
{
   uint32_t a = gDP.colorImage.address;
   int w = (int)gDP.colorImage.width;
   int rows;

   if (gDP.colorImage.size != G_IM_SIZ_16b || w <= 0 || w > SOFT_WIDTH || (a & 3))
   {
      if (soft_off_addr)
         soft_unbind();
      return;
   }
   if (!soft_main_addr && w == SOFT_WIDTH)
      soft_main_addr = a;
   if (w == SOFT_WIDTH && a == soft_main_addr)
   {
      if (soft_off_addr)
         soft_unbind();
      soft_main_dirty = 1;
      return;
   }
   if (a == soft_off_addr && w == soft_off_w)
      return;
   if (soft_off_addr)
      soft_unbind();

   rows = SOFT_WIDTH * SOFT_HEIGHT / w;
   if ((RDRAMSize - a) / (2u * w) < (uint32_t)rows)
      rows = (int)((RDRAMSize - a) / (2u * w));
   if (rows <= 0)
      return;
   soft_flush();
   if (soft_main_dirty && soft_frame_fits(soft_main_addr))
      soft_store_rdram(soft_main_addr, SOFT_WIDTH, SOFT_HEIGHT);
   soft_main_dirty = 0;
   memcpy(soft_save_color, soft_color, sizeof(soft_color));
   memcpy(soft_save_depth, soft_depth, sizeof(soft_depth));
   soft_load_rdram(a, w, rows);
   soft_target_init(&soft_target, soft_color, soft_depth, w, rows);
   soft_off_addr = a;
   soft_off_w = w;
   soft_off_rows = 0;
}

void soft_rdram_read(uint32_t addr)
{
   if (soft_wb_addr && addr - soft_wb_addr < SOFT_FRAME_BYTES)
      soft_wait_ahead();
   if (soft_main_dirty && !soft_off_addr && addr - soft_main_addr < SOFT_FRAME_BYTES
         && soft_frame_fits(soft_main_addr))
   {
      soft_flush();
      soft_store_rdram(soft_main_addr, SOFT_WIDTH, SOFT_HEIGHT);
      soft_main_dirty = 0;
   }
}

static void soft_present_back(void)
{
   soft_present_to(soft_fronts[soft_shown ^ 1]);
   soft_write_back();
}

/* Draws the frame on the worker while emulation runs on, showing the previous frame meanwhile. */
static int soft_present_ahead(void)
{
   soft_wait_ahead();
   soft_shown ^= 1;
   soft_texcache_hold();
   if (!soft_mt_async(soft_q, &soft_target, soft_present_back))
   {
      soft_texcache_unhold();
      soft_shown ^= 1;
      return 0;
   }
   soft_q = soft_q == &soft_queues[0] ? &soft_queues[1] : &soft_queues[0];
   soft_queue_reset(soft_q);
   return 1;
}

void soft_present(void)
{
   static bool logged;

   soft_wait_ahead();
   if (soft_off_addr)
      soft_unbind();
   soft_capture_wb_addr();
   soft_main_addr = 0;
   soft_main_dirty = 0;
   if (!soft_async || !soft_present_ahead())
   {
      soft_flush();
      soft_present_to(soft_fronts[soft_shown]);
      soft_write_back();
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
   if (soft_off_addr && soft_target.sy1 > soft_off_rows)
      soft_off_rows = soft_target.sy1;
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

static SoftCombineProgram soft_prog;

/* Recently compiled combiner programs, keyed by mux and flags and replaced round robin. */
#define SOFT_PROG_CACHE 16
static struct
{
   uint64_t mux;
   int flags;
   SoftCombineProgram prog;
} soft_prog_cache[SOFT_PROG_CACHE];
static int soft_prog_cached, soft_prog_next;

/* Sets soft_prog and soft_combine_flags for the current combiner, compiling only on a miss. */
static void soft_update_combiner(void)
{
   uint64_t mux;
   int flags = Combiner_Flags(), i;

   if (gDP.otherMode.cycleType == G_CYC_COPY)
      mux = EncodeCombineMode(0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0);
   else if (gDP.otherMode.cycleType == G_CYC_FILL)
      mux = EncodeCombineMode(0, 0, 0, SHADE, 0, 0, 0, 1, 0, 0, 0, SHADE, 0, 0, 0, 1);
   else
      mux = gDP.combine.mux;

   for (i = 0; i < soft_prog_cached; i++)
      if (soft_prog_cache[i].mux == mux && soft_prog_cache[i].flags == flags)
      {
         soft_combine_flags = flags;
         soft_prog = soft_prog_cache[i].prog;
         return;
      }
   Combiner_Set(mux, flags);
   soft_combine_compile(&soft_dmux, (soft_combine_flags & SC_2CYCLE) != 0, &soft_prog);
   i = soft_prog_next;
   soft_prog_next = (soft_prog_next + 1) % SOFT_PROG_CACHE;
   if (soft_prog_cached < SOFT_PROG_CACHE)
      soft_prog_cached++;
   soft_prog_cache[i].mux = mux;
   soft_prog_cache[i].flags = flags;
   soft_prog_cache[i].prog = soft_prog;
}


/* Re-decodes tile 0 or 1 when texture, tile or TMEM state changed, or a rectangle swapped its tile. */
static const SoftTex *soft_texture(uint32_t t)
{
   if ((gSP.changed & CHANGED_TEXTURE) || (gDP.changed & (CHANGED_TILE | CHANGED_TMEM)))
   {
      soft_current_tex[0] = soft_current_tex[1] = NULL;
      gSP.changed &= ~CHANGED_TEXTURE;
      gDP.changed &= ~(CHANGED_TILE | CHANGED_TMEM);
   }
   if (soft_current_tex[t] == NULL || soft_current_tile[t] != gSP.textureTile[t])
   {
      /* A lookup may evict unpinned entries, so keep the other tile's texture alive. */
      soft_texcache_pin(soft_current_tex[t ^ 1]);
      soft_current_tex[t] = soft_texcache_get(t);
      soft_current_tile[t] = gSP.textureTile[t];
   }
   return soft_current_tex[t];
}

static int32_t soft_fix16(float v)
{
   return (int32_t)(v * 65536.0f + (v < 0.0f ? -0.5f : 0.5f));
}

/* Maps tile 0 texel coordinates onto tile 1; base is the half-texel offset triangles add. */
static void soft_tex1_map(SoftPipe *p, float base)
{
   SoftTexCoords c0, c1;
   float ks, kt;

   soft_tex_coords(0, &c0);
   soft_tex_coords(1, &c1);
   ks = c1.shift_s / c0.shift_s;
   kt = c1.shift_t / c0.shift_t;
   p->tex1_ks = soft_fix16(ks);
   p->tex1_kt = soft_fix16(kt);
   p->tex1_os = soft_fix16(base - gSP.textureTile[1]->fuls - (base - gSP.textureTile[0]->fuls) * ks);
   p->tex1_ot = soft_fix16(base - gSP.textureTile[1]->fult - (base - gSP.textureTile[0]->fult) * kt);
}

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
static void build_pipe(SoftPipe *p, bool *textured, float tex_base)
{
   bool zbuf;
   static bool logger_set;

   if (!logger_set)
   {
      soft_combine_set_logger(soft_log);
      logger_set = true;
   }
   soft_update_combiner();

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
   p->filter = gDP.otherMode.textureFilter != G_TF_POINT && gDP.otherMode.cycleType < G_CYC_COPY;
   /* Depth-less draws are usually HUD and text, so only depth-tested draws dither. */
   p->dither = p->filter && soft_tex_filter == 0 && p->z_test;
   if (soft_tex_filter == 2 || p->dither)
      p->filter = 0;
   p->fog_color = soft_u8(gDP.fogColor.r) | (soft_u8(gDP.fogColor.g) << 8) | (soft_u8(gDP.fogColor.b) << 16);
   p->prim[0] = soft_u8(gDP.primColor.r); p->prim[1] = soft_u8(gDP.primColor.g);
   p->prim[2] = soft_u8(gDP.primColor.b); p->prim[3] = soft_u8(gDP.primColor.a);
   p->env[0]  = soft_u8(gDP.envColor.r);  p->env[1]  = soft_u8(gDP.envColor.g);
   p->env[2]  = soft_u8(gDP.envColor.b);  p->env[3]  = soft_u8(gDP.envColor.a);
   p->prim_lod = soft_u8(gDP.primColor.l);
   soft_folded = soft_combine_fold(&soft_prog, p->prim, p->env, p->prim_lod, &soft_fold);

   *textured = gDP.otherMode.cycleType != G_CYC_FILL && soft_prog.uses_tex;
   if (*textured)
   {
      p->tex = soft_texture(0);
      if (p->tex == NULL)
      {
         *textured = false;
         soft_prog.uses_tex = 0;
      }
      else if (soft_prog.uses_tex1 && gSP.textureTile[1] != NULL)
      {
         p->tex1 = soft_texture(1);
         if (p->tex1 != NULL)
            soft_tex1_map(p, tex_base);
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

static void soft_build_pipe(SoftPipe *p, bool *textured, float tex_base)
{
   SOFT_TIME_BEGIN(t0);
   build_pipe(p, textured, tex_base);
   SOFT_TIME_END(SOFT_T_PIPE, t0);
}

/* Projects one vertex and applies the per-draw colour, fog and texture coordinate rules. */
static bool soft_finish_vert(struct SPVertex *v, const SoftViewport *vp, int shift, bool flat,
      int fog, const SoftTexCoords *tc, SoftVert *out)
{
   if (!soft_vertex_to_screen(v, vp, shift, out))
      return false;
   if (flat)
      soft_flat_colors(v, out);
   if (fog)
      out->fog = soft_vertex_fog(v);
   if (tc)
   {
      out->s = v->s * tc->scale_s + tc->off_s;
      out->t = v->t * tc->scale_t + tc->off_t;
   }
   return true;
}

/* Screen vertices shared by a draw's unclipped triangles; a stamp marks those projected this draw. */
static SoftVert soft_proj[VERTBUFF_SIZE];
static uint32_t soft_proj_stamp[VERTBUFF_SIZE];
static uint32_t soft_stamp;

static void soft_draw_list(const uint8_t *elements, uint32_t count)
{
   SoftViewport vp;
   SoftPipe pipe;
   SoftTexCoords tc;
   const SoftTexCoords *tcp = NULL;
   uint32_t i;
   bool textured;
   int shift = (soft_bind(), soft_shift());
   bool flat = (gSP.geometryMode & G_SHADE) == 0
            || (gSP.geometryMode & G_SHADING_SMOOTH) == 0;
   SOFT_TIME_BEGIN(t0);

   soft_viewport(&vp);
   soft_update_scissor(shift);
   soft_build_pipe(&pipe, &textured, 0.5f);
   if (textured)
   {
      soft_tex_coords(0, &tc);
      tcp = &tc;
   }
   /* Stamp 0 never matches, so wrapping skips it. */
   if (++soft_stamp == 0)
   {
      memset(soft_proj_stamp, 0, sizeof(soft_proj_stamp));
      soft_stamp = 1;
   }

   for (i = 0; i + 2 < count; i += 3)
   {
      struct SPVertex *in[3];
      unsigned idx[3];
      int k, n;

      for (k = 0; k < 3; k++)
      {
         idx[k] = elements ? elements[i + k] : i + k;
         in[k] = &OGL.triangles.vertices[idx[k]];
      }
      if (in[0]->z + in[0]->w >= 0.0f && in[1]->z + in[1]->w >= 0.0f && in[2]->z + in[2]->w >= 0.0f)
      {
         const SoftVert *sv[3];
         for (k = 0; k < 3; k++)
         {
            unsigned j = idx[k];
            if (soft_proj_stamp[j] != soft_stamp)
            {
               soft_proj_stamp[j] = soft_stamp;
               if (!soft_finish_vert(in[k], &vp, shift, flat, pipe.fog, tcp, &soft_proj[j]))
                  soft_proj[j].invw = 0.0f;
            }
            if (soft_proj[j].invw == 0.0f)
               break;
            sv[k] = &soft_proj[j];
         }
         if (k == 3)
            soft_record_tri(sv[0], sv[1], sv[2], &pipe);
         continue;
      }
      {
         struct SPVertex tri[3], poly[SOFT_CLIP_MAX];
         SoftVert sv[SOFT_CLIP_MAX];

         for (k = 0; k < 3; k++)
            tri[k] = *in[k];
         n = soft_clip_near(tri, poly);
         for (k = 0; k < n; k++)
            if (!soft_finish_vert(&poly[k], &vp, shift, flat, pipe.fog, tcp, &sv[k]))
               break;
         if (k < n)
            continue;
         for (k = 1; k + 1 < n; k++)
            soft_record_tri(&sv[0], &sv[k], &sv[k + 1], &pipe);
      }
   }
   SOFT_TIME_END(SOFT_T_TRIS, t0);
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

static uint16_t soft_pack565(const float *c)
{
   int r = (int)(c[0] * 31.0f + 0.5f);
   int g = (int)(c[1] * 63.0f + 0.5f);
   int b = (int)(c[2] * 31.0f + 0.5f);
   return (uint16_t)((r << 11) | (g << 5) | b);
}

static void soft_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
   if (soft_queue_full(soft_q))
      soft_flush();
   soft_queue_fill(soft_q, &soft_target, x0, y0, x1, y1, c);
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

/* Draws a strip of screen-space vertices from S2DEX object and background rectangles and LLE triangles. */
void OGL_DrawLLETriangle(uint32_t _numVtx)
{
   SoftPipe pipe;
   SoftTexCoords tc;
   SoftVert v[3];
   bool textured;
   int shift = soft_shift();
   uint32_t i;

   OGL.triangles.num = 0;
   if (_numVtx < 3 || gDP.otherMode.cycleType == G_CYC_FILL)
      return;
   soft_bind();
   soft_update_scissor(shift);
   soft_build_pipe(&pipe, &textured, 0.5f);
   pipe.cull = SOFT_CULL_NONE;
   if (textured)
      soft_tex_coords(0, &tc);
   for (i = 0; i < _numVtx; i++)
   {
      const struct SPVertex *in = &OGL.triangles.vertices[i];
      SoftVert *out = &v[i % 3];
      float zf = in->z * 32767.5f + 32767.5f;
      float s = in->s, t = in->t;

      if (gDP.otherMode.texturePersp == 0)
      {
         s *= 2.0f;
         t *= 2.0f;
      }
      soft_rect_vert(out, in->x, in->y, shift,
            (uint16_t)(zf < 0.0f ? 0.0f : (zf > 65535.0f ? 65535.0f : zf)), 0.0f, 0.0f);
      out->r = soft_u8(in->r); out->g = soft_u8(in->g);
      out->b = soft_u8(in->b); out->a = soft_u8(in->a);
      if (textured)
      {
         out->s = s * tc.scale_s + tc.off_s;
         out->t = t * tc.scale_t + tc.off_t;
      }
      if (in->w > 0.0f)
         out->invw = 1.0f / in->w;
      if (i >= 2)
         soft_record_tri(&v[(i - 2) % 3], &v[(i - 1) % 3], &v[i % 3], &pipe);
   }
}

void OGL_DrawRect(int ulx, int uly, int lrx, int lry, float *color)
{
   int shift;

   soft_bind();
   shift = soft_shift();

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
      soft_build_pipe(&pipe, &textured, 0.0f);
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
   int shift;

   soft_bind();
   shift = soft_shift();
   soft_update_scissor(shift);
   soft_build_pipe(&pipe, &textured, 0.0f);
   pipe.cull = SOFT_CULL_NONE;
   /* Rectangles carry text and HUD, so they keep the exact filter. */
   if (pipe.dither)
   {
      pipe.dither = 0;
      pipe.filter = 1;
   }
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
   if (soft_queue_full(soft_q))
      soft_flush();
   soft_queue_clear_depth(soft_q);
}

void OGL_ClearColorBuffer(float *color)
{
   soft_bind();
   soft_update_scissor(soft_shift());
   if (soft_queue_full(soft_q))
      soft_flush();
   soft_queue_clear_color(soft_q, soft_pack565(color));
}

void soft_ogl_stop(void)
{
   soft_off_addr = 0;
   soft_main_addr = 0;
   soft_main_dirty = 0;
   soft_wb_addr = 0;
   soft_mt_stop();
   soft_texcache_unhold();
   soft_queue_reset(soft_q);
   soft_texcache_release();
   soft_current_tex[0] = soft_current_tex[1] = NULL;
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
