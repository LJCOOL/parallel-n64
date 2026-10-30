#include "soft_xform.h"

/* Keeps 28.4 coordinates far from int32 overflow; the rasteriser clips to scissor. */
#define SOFT_COORD_LIMIT 1048576.0f

int soft_scale_shift(uint32_t color_image_width)
{
   return color_image_width > 320 ? 2 : 1;
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
   out->s    = (int32_t)(in->s * 65536.0f);
   out->t    = (int32_t)(in->t * 65536.0f);
   out->invw = rw;
   return true;
}
