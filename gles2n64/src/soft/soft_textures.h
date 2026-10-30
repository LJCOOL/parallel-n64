#ifndef SOFT_TEXTURES_H
#define SOFT_TEXTURES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maps a tile's s, t to texel units: texel = s * scale + offset. */
typedef struct
{
   float scale_s, scale_t;
   float off_s, off_t;
   float shift_s, shift_t;   /* shift scale alone, for rectangles */
} SoftTexCoords;

void soft_tex_coords(uint32_t tile, SoftTexCoords *out);

#ifdef __cplusplus
}
#endif

#endif
