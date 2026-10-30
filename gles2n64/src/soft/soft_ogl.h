#ifndef SOFT_OGL_H
#define SOFT_OGL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Clears the render target and the presented frame. */
void soft_ogl_init(void);

/* Publishes the finished frame for video_cb. */
void soft_present(void);

#ifdef __cplusplus
}
#endif

#endif
