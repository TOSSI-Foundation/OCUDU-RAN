#pragma once
#include <stdio.h>

enum { HW = 0, PHY, UTIL };
#ifdef __cplusplus
extern "C" {
#endif
void rfsim_log(int level, int comp, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif
#define OAILOG_ERR   0
#define OAILOG_WARN  1
#define OAILOG_INFO  2
#define OAILOG_DEBUG 3
#define LOG_E(c, ...) rfsim_log(OAILOG_ERR,   c, __VA_ARGS__)
#define LOG_W(c, ...) rfsim_log(OAILOG_WARN,  c, __VA_ARGS__)
#define LOG_A(c, ...) rfsim_log(OAILOG_INFO,  c, __VA_ARGS__)
#define LOG_I(c, ...) rfsim_log(OAILOG_INFO,  c, __VA_ARGS__)
#define LOG_D(c, ...) rfsim_log(OAILOG_DEBUG, c, __VA_ARGS__)
#define LOG_M(...)    do {} while (0)
