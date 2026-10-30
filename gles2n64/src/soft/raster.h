#ifndef SOFT_RASTER_H
#define SOFT_RASTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_CULL_NONE  0
#define SOFT_CULL_BACK  1   /* clockwise on screen (y down) */
#define SOFT_CULL_FRONT 2   /* counter-clockwise on screen */

/* Colour and depth target; scissor is half-open and clamped to the size. */
typedef struct
{
   uint16_t *color;
   uint16_t *depth;
   int width, height;
   int sx0, sy0, sx1, sy1;
} SoftTarget;

/* Screen-space vertex: x, y in 28.4 fixed point, s, t in 16.16 texels. */
typedef struct
{
   int32_t x, y;
   uint16_t z;
   uint8_t r, g, b, a;
   int32_t s, t;
   float invw;
} SoftVert;

/* A triangle after setup, with vertices in the order they were submitted. */
typedef struct
{
   const SoftVert *v[3];
} SoftTri;

struct SoftPipe;

/* Fills pixels [x0, x1) of row y. */
typedef void (*SoftSpanFn)(const SoftTarget *t, const struct SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1);

typedef struct SoftPipe
{
   SoftSpanFn span;
   int cull;
} SoftPipe;

void soft_target_init(SoftTarget *t, uint16_t *color, uint16_t *depth, int w, int h);
void soft_set_scissor(SoftTarget *t, int x0, int y0, int x1, int y1);
void soft_draw_tri(SoftTarget *t, const SoftVert *v0, const SoftVert *v1,
      const SoftVert *v2, const SoftPipe *p);

/* Writes v0's colour as RGB565. */
void soft_span_flat(const SoftTarget *t, const SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1);

#ifdef __cplusplus
}
#endif

#endif
