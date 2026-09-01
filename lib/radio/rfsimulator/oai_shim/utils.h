#pragma once
#include <stdlib.h>
#include <stdio.h>
#define sizeofArray(a) (sizeof(a) / sizeof((a)[0]))
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif
static inline void *malloc_or_fail(size_t sz)
{
  void *p = malloc(sz);
  if (!p) { fprintf(stderr, "malloc(%zu) failed\n", sz); abort(); }
  return p;
}
static inline void *calloc_or_fail(size_t n, size_t sz)
{
  void *p = calloc(n, sz);
  if (!p) { fprintf(stderr, "calloc(%zu,%zu) failed\n", n, sz); abort(); }
  return p;
}
