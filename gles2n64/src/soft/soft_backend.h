#ifndef SOFT_BACKEND_H
#define SOFT_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_WIDTH  160
#define SOFT_HEIGHT 120

/* The finished 160x120 RGB565 frame handed to video_cb. */
const uint16_t *soft_frame(void);

/* Display-list timing, reported every SOFT_STATS_FRAMES frames. */
void soft_stats_begin(void);
void soft_stats_end(void);
void soft_stats_frame(void);

#ifdef __cplusplus
}
#endif

#endif
