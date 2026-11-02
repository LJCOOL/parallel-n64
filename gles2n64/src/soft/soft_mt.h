#ifndef SOFT_MT_H
#define SOFT_MT_H

#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the band-1 worker; returns 1 when running, 0 when replay stays on the caller. */
int soft_mt_start(void);
/* Joins the worker if one is running. */
void soft_mt_stop(void);
/* Replays both bands of q, split across the worker when available, and returns when done. */
void soft_mt_replay(const SoftQueue *q, SoftTarget *t);
/* Replays all of q on the worker, then calls after there; returns 0 without a worker. */
int soft_mt_async(const SoftQueue *q, SoftTarget *t, void (*after)(void));
/* Waits for the job started by soft_mt_async, if any. */
void soft_mt_wait(void);

#ifdef __cplusplus
}
#endif

#endif
