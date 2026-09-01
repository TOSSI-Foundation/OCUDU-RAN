#pragma once
#include <string.h>
#include "common/config/config_paramdesc.h"
#include "utils.h"

#ifdef __cplusplus
extern "C" {
#endif
struct configmodule_interface { int rtflags; };
#define config_get_if() rfsim_config_get_if()
configmodule_interface_t *rfsim_config_get_if(void);

void rfsim_config_set(const char *name, const char *value);

int config_get(configmodule_interface_t *cfg, paramdef_t *params, int numparams, const char *prefix);
int config_getlist(configmodule_interface_t *cfg, paramlist_def_t *list, paramdef_t *params, int numparams, const char *prefix);
int config_paramidx_fromname(const paramdef_t *params, int numparams, const char *name);
paramdef_t *config_get_paramdef_from_name(paramdef_t *params, int numparams, const char *name);
#define gpd(pd_array, nump, name) config_get_paramdef_from_name(pd_array, nump, name)
#ifdef __cplusplus
}
#endif
