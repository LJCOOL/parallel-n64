#ifndef SOFT_OGL_H
#define SOFT_OGL_H

#include "../Combiner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Combiner state recorded by ShaderCombiner_Set. */
extern DecodedMux soft_dmux;
extern int soft_combine_flags;

/* Clears the render target and the presented frame. */
void soft_ogl_init(void);

/* Publishes the finished frame for video_cb. */
void soft_present(void);

#ifdef __cplusplus
}
#endif

#endif
