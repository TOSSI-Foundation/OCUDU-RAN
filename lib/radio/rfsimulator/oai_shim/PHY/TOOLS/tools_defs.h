#pragma once
#include <stdint.h>
#include <math.h>
typedef struct complex8  { int8_t  r, i; } c8_t;
typedef struct complex16 { int16_t r, i; } c16_t;
typedef struct complex32 { int32_t r, i; } c32_t;
typedef struct complex64 { int64_t r, i; } c64_t;
typedef struct complexf  { float   r, i; } cf_t;
#ifdef __cplusplus
extern "C" {
#endif
int32_t signal_energy(int32_t *input, uint32_t length);
#ifdef __cplusplus
}
#endif
