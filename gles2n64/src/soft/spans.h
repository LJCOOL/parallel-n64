#ifndef SOFT_SPANS_H
#define SOFT_SPANS_H

#include "raster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decal surfaces win depth ties within this many 16-bit Z units. */
#define SOFT_DECAL_BIAS 32

/* Span variants; Z variants test less-or-equal against the depth buffer. */
void soft_span_flat(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);
void soft_span_gouraud(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);
void soft_span_gouraud_z(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);
void soft_span_tex_z(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);
/* Full pipeline: depth, texture, combiner, fog, alpha compare and blender. */
void soft_span_combine(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);
#ifdef __ARM_NEON
/* Same output as soft_span_combine, four pixels per step, inlining non-generic combine modes. */
void soft_span_combine_neon(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);
#define SOFT_SPAN_COMBINE soft_span_combine_neon
#else
#define SOFT_SPAN_COMBINE soft_span_combine
#endif
void soft_span_tex_gouraud_z(const SoftTarget *t, const SoftPipe *p, const SoftTri *tri, int y, int x0, int x1);

#ifdef __cplusplus
}
#endif

#endif
