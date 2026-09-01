#pragma once
#include <stdio.h>
#include <stdlib.h>
#define AssertFatal(cOND, ...)                                       \
  do { if (!(cOND)) {                                                \
    fprintf(stderr, "Assertion (%s) failed at %s:%d\n",              \
            #cOND, __FILE__, __LINE__);                              \
    fprintf(stderr, __VA_ARGS__); abort(); } } while (0)
#define DevAssert(cOND) AssertFatal(cOND, "DevAssert\n")
#define AssertError(c, v, ...) AssertFatal(c, __VA_ARGS__)
