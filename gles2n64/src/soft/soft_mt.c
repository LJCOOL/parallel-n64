#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif

#include "soft_mt.h"
#include "soft_stats.h"

static pthread_t worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static int running, quit;
static unsigned gen, done_gen;
static const SoftQueue *job_q;
static SoftTarget *job_t;

static void *worker_main(void *arg)
{
   unsigned seen = 0;
   (void)arg;
   soft_stats_set_lane(1);
   for (;;)
   {
      const SoftQueue *q;
      SoftTarget *t;

      pthread_mutex_lock(&lock);
      while (gen == seen && !quit)
         pthread_cond_wait(&start_cv, &lock);
      if (quit)
      {
         pthread_mutex_unlock(&lock);
         return NULL;
      }
      seen = gen;
      q = job_q;
      t = job_t;
      pthread_mutex_unlock(&lock);

      {
         SOFT_TIME_BEGIN(t0);
         soft_queue_replay(q, t, 1);
         SOFT_TIME_END(SOFT_T_BAND1, t0);
      }

      pthread_mutex_lock(&lock);
      done_gen = seen;
      pthread_cond_signal(&done_cv);
      pthread_mutex_unlock(&lock);
   }
}

int soft_mt_start(void)
{
   if (running)
      return 1;
   quit = 0;
   gen = done_gen = 0;
   if (pthread_create(&worker, NULL, worker_main, NULL) != 0)
      return 0;
#ifdef __linux__
   {
      /* The caller (emulation thread) takes CPU 0 and the worker CPU 1, so they never share. */
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(1, &set);
      pthread_setaffinity_np(worker, sizeof(set), &set);
      CPU_ZERO(&set);
      CPU_SET(0, &set);
      pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
   }
#endif
   running = 1;
   return 1;
}

void soft_mt_stop(void)
{
   if (!running)
      return;
   pthread_mutex_lock(&lock);
   quit = 1;
   pthread_cond_signal(&start_cv);
   pthread_mutex_unlock(&lock);
   pthread_join(worker, NULL);
   running = 0;
}

void soft_mt_replay(const SoftQueue *q, SoftTarget *t)
{
   unsigned g;

   if (!running)
   {
      soft_queue_replay(q, t, 0);
      soft_queue_replay(q, t, 1);
      return;
   }
   pthread_mutex_lock(&lock);
   job_q = q;
   job_t = t;
   g = ++gen;
   pthread_cond_signal(&start_cv);
   pthread_mutex_unlock(&lock);

   {
      SOFT_TIME_BEGIN(t0);
      soft_queue_replay(q, t, 0);
      SOFT_TIME_END(SOFT_T_BAND0, t0);
   }
   {
      SOFT_TIME_BEGIN(t1);
      pthread_mutex_lock(&lock);
      while (done_gen != g)
         pthread_cond_wait(&done_cv, &lock);
      pthread_mutex_unlock(&lock);
      SOFT_TIME_END(SOFT_T_WAIT, t1);
   }
}
