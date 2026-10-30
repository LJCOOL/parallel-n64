#ifndef SOFT_XFORM_H
#define SOFT_XFORM_H

#include <boolean.h>

#include "raster.h"
#include "../../../Graphics/RSP/gSP_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NDC to colour-image pixels: screen = ndc * s + t; z maps NDC to 0..65535. */
typedef struct
{
   float sx, sy, tx, ty, sz, tz;
} SoftViewport;

/* Right shift from colour-image pixels to the 160x120 target. */
int soft_scale_shift(uint32_t color_image_width);

/* Projects a clip-space vertex; false when it is at or behind the eye (w <= 0). */
bool soft_vertex_to_screen(const struct SPVertex *in, const SoftViewport *vp,
      int shift, SoftVert *out);

#ifdef __cplusplus
}
#endif

#endif
