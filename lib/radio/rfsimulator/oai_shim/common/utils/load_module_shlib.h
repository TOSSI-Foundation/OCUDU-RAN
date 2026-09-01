#pragma once
#ifdef __cplusplus
extern "C" {
#endif

static inline void *get_shlibmodule_fptr(const char *m, const char *f) { (void)m; (void)f; return 0; }
#ifdef __cplusplus
}
#endif
