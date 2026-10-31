#ifndef SOFT_QUEUE_H
#define SOFT_QUEUE_H

#include <stdint.h>

#include "raster.h"
#include "combine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_QUEUE_MAX 4096

typedef enum
{
   SOFT_CMD_TRI,
   SOFT_CMD_FILL,
   SOFT_CMD_CLEAR_COLOR,
   SOFT_CMD_CLEAR_DEPTH
} SoftCmdKind;

/* One recorded draw; triangles carry copies of everything that may change before replay. */
typedef struct
{
   SoftCmdKind kind;
   int sx0, sy0, sx1, sy1;
   union
   {
      struct
      {
         SoftVert v[3];
         SoftPipe pipe;
         SoftCombineProgram prog;
         SoftTriSetup setup;
      } tri;
      struct
      {
         int x0, y0, x1, y1;
         uint16_t c;
      } fill;
   } u;
} SoftCmd;

typedef struct SoftQueue
{
   SoftCmd cmd[SOFT_QUEUE_MAX];
   int count;
} SoftQueue;

void soft_queue_reset(SoftQueue *q);
int soft_queue_full(const SoftQueue *q);
void soft_queue_tri(SoftQueue *q, const SoftTarget *t, const SoftVert *v0,
      const SoftVert *v1, const SoftVert *v2, const SoftPipe *p);
void soft_queue_fill(SoftQueue *q, const SoftTarget *t, int x0, int y0, int x1, int y1, uint16_t c);
void soft_queue_clear_color(SoftQueue *q, uint16_t c);
void soft_queue_clear_depth(SoftQueue *q);
/* Runs every command in order, touching only rows of band (0, 1, or -1 for all). */
void soft_queue_replay(const SoftQueue *q, SoftTarget *t, int band);

#ifdef __cplusplus
}
#endif

#endif
