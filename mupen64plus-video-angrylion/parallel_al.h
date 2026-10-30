#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef PARALLEL_MAX_WORKERS
#define PARALLEL_MAX_WORKERS 64u
#endif

void parallel_alinit(uint32_t num);

void parallel_run(void task(uint32_t));

uint32_t parallel_num_workers(void);

void parallel_close(void);

#ifdef __cplusplus
}
#endif
