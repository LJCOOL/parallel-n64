#ifndef SOFT_RASTER_H
#define SOFT_RASTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screen bands alternate every 1 << SOFT_ROW_SHIFT rows between two render threads. */
#define SOFT_ROW_SHIFT 3

static inline int soft_row_in_band(int y, int band)
{
   return band < 0 || ((y >> SOFT_ROW_SHIFT) & 1) == band;
}

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

/* Screen-space vertex: x, y in 28.4 fixed point, s, t in texels. */
typedef struct
{
   int32_t x, y;
   uint16_t z;
   uint8_t r, g, b, a;
   uint8_t fog;      /* fog factor, 0 = none, 255 = full fog colour */
   float s, t;
   float invw;
} SoftVert;

/* Attribute plane: value at pixel centre (px, py) is c + dx * px + dy * py. */
typedef struct
{
   float dx, dy, c;
} SoftGrad;

/* A triangle after setup; s and t planes hold s/w and t/w for perspective. */
typedef struct
{
   const SoftVert *v[3];
   SoftGrad z, r, g, b, a, f, sw, tw, w;
} SoftTri;

struct SoftTex;

struct SoftPipe;

/* Fills pixels [x0, x1) of row y. */
typedef void (*SoftSpanFn)(const SoftTarget *t, const struct SoftPipe *p,
      const SoftTri *tri, int y, int x0, int x1);

typedef struct SoftPipe
{
   SoftSpanFn span;
   int cull;
   int z_test;                /* compare against the depth buffer */
   int z_update;              /* write depth when the test passes */
   int decal;                 /* decal Z mode: bias the compare */
   int alpha_test;            /* drop texels with alpha below alpha_ref */
   uint8_t alpha_ref;
   const struct SoftTex *tex;
   const void *prog;          /* SoftCombineProgram for soft_span_combine */
   uint8_t prim[4], env[4];
   uint8_t prim_lod;
   uint32_t fog_color;
   int fog;
   int blend;                 /* SoftBlendMode */
} SoftPipe;

/* Edge a->b; top-left edges own their boundary pixels. */
typedef struct
{
   int64_t value;   /* at the bounding box origin pixel centre */
   int64_t step_x, step_y;
} SoftEdge;

/* A culled, clipped and set-up triangle, ready to raster any band without repeating setup. */
typedef struct
{
   SoftTri tri;
   SoftEdge e[3];
   int x0, y0, x1, y1;
} SoftTriSetup;

void soft_target_init(SoftTarget *t, uint16_t *color, uint16_t *depth, int w, int h);
void soft_set_scissor(SoftTarget *t, int x0, int y0, int x1, int y1);
void soft_draw_tri(SoftTarget *t, const SoftVert *v0, const SoftVert *v1,
      const SoftVert *v2, const SoftPipe *p);
/* Sets up a triangle against t's scissor; returns 0 when nothing is drawn. Keeps pointers to the vertices. */
int soft_tri_prepare(SoftTriSetup *s, const SoftTarget *t, const SoftVert *v0,
      const SoftVert *v1, const SoftVert *v2, const SoftPipe *p);
/* Rasters rows of band 0 or 1 of a prepared triangle; band -1 draws every row. */
void soft_tri_raster(const SoftTriSetup *s, SoftTarget *t, const SoftPipe *p, int band);
/* Draws only rows of band 0 or 1; band -1 draws every row. */
void soft_draw_tri_band(SoftTarget *t, const SoftVert *v0, const SoftVert *v1,
      const SoftVert *v2, const SoftPipe *p, int band);

#ifdef __cplusplus
}
#endif

#endif
