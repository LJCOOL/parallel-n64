#ifndef SOFT_TEST_H
#define SOFT_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int test_failures;

#define CHECK(cond) do { \
   if (!(cond)) { \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      test_failures++; \
   } \
} while (0)

#define RUN(fn) do { fn(); } while (0)

#define TEST_EXIT() do { \
   if (test_failures) { fprintf(stderr, "%d check(s) failed\n", test_failures); return 1; } \
   return 0; \
} while (0)

#endif
