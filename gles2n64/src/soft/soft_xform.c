#include "soft_xform.h"

/* Keeps 28.4 coordinates far from int32 overflow; the rasteriser clips to scissor. */
#define SOFT_COORD_LIMIT 1048576.0f

int soft_scale_shift(uint32_t color_image_width)
{
   return color_image_width > 320 ? 1 : 0;
}

static float clampf(float v, float lo, float hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

bool soft_vertex_to_screen(const struct SPVertex *in, const SoftViewport *vp,
      int shift, SoftVert *out)
{
   float rw, scale, x, y, z;

   if (!(in->w > 0.0f))
      return false;

   rw    = 1.0f / in->w;
   scale = 16.0f / (float)(1 << shift);
   x     = (in->x * rw * vp->sx + vp->tx) * scale;
   y     = (in->y * rw * vp->sy + vp->ty) * scale;
   z     = in->z * rw * vp->sz + vp->tz;

   out->x    = (int32_t)clampf(x, -SOFT_COORD_LIMIT, SOFT_COORD_LIMIT);
   out->y    = (int32_t)clampf(y, -SOFT_COORD_LIMIT, SOFT_COORD_LIMIT);
   out->z    = (uint16_t)clampf(z, 0.0f, 65535.0f);
   out->r    = (uint8_t)clampf(in->r * 255.0f, 0.0f, 255.0f);
   out->g    = (uint8_t)clampf(in->g * 255.0f, 0.0f, 255.0f);
   out->b    = (uint8_t)clampf(in->b * 255.0f, 0.0f, 255.0f);
   out->a    = (uint8_t)clampf(in->a * 255.0f, 0.0f, 255.0f);
   out->s    = in->s;
   out->t    = in->t;
   out->invw = rw;
   return true;
}

static void lerp_vertex(const struct SPVertex *a, const struct SPVertex *b, float t, struct SPVertex *o)
{
#define L(f) o->f = a->f + (b->f - a->f) * t
   L(x); L(y); L(z); L(w);
   L(r); L(g); L(b); L(a);
   L(flat_r); L(flat_g); L(flat_b); L(flat_a);
   L(s); L(t);
#undef L
   o->HWLight = a->HWLight;
   o->flag    = a->flag;
   o->clip    = 0;
}

int soft_clip_near(const struct SPVertex in[3], struct SPVertex out[SOFT_CLIP_MAX])
{
   int i, n = 0;
   for (i = 0; i < 3; i++)
   {
      const struct SPVertex *a = &in[i], *b = &in[(i + 1) % 3];
      float da = a->z + a->w, db = b->z + b->w;
      if (da >= 0.0f)
         out[n++] = *a;
      if ((da >= 0.0f) != (db >= 0.0f))
         lerp_vertex(a, b, da / (da - db), &out[n++]);
   }
   return n;
}
