#ifndef SOFT_BACKEND_H
#define SOFT_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_WIDTH  320
#define SOFT_HEIGHT 240

/* The finished 320x240 XRGB8888 frame handed to video_cb. */
const uint32_t *soft_frame(void);

/* Display-list timing, reported every SOFT_STATS_FRAMES frames. */
void soft_stats_begin(void);
void soft_stats_end(void);
void soft_stats_frame(void);

/* Texture filtering: 0 dithered single tap, 1 N64 three-point, 2 nearest. */
void soft_set_tex_filter(int mode);
/* Draws each frame on the second core during the next one's emulation, adding a frame of latency. */
void soft_set_async(int on);

#ifdef __cplusplus
}
#endif

#endif
