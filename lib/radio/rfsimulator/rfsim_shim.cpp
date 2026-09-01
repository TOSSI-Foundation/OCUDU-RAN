// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "rfsim_shim.h"

#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include "PHY/TOOLS/tools_defs.h"
extern "C" {
#include "radio/rfsimulator/rfsimulator.h"
}

static int rfsim_log_level = OAILOG_INFO;
static void (*rfsim_log_sink)(int, const char *) = nullptr;

namespace ocudu {
void rfsim_set_log_level(int level) { rfsim_log_level = level; }
void rfsim_set_log_sink(void (*sink)(int, const char *)) { rfsim_log_sink = sink; }
}

void rfsim_log(int level, int comp, const char *fmt, ...)
{
  (void)comp;
  if (level > rfsim_log_level) {
    return;
  }
  char    msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  if (rfsim_log_sink) {
    rfsim_log_sink(level, msg);
    return;
  }
  static const char *tag[] = {"E", "W", "I", "D"};
  fprintf(stderr, "[rfsim %s] %s", tag[level], msg);
}

static std::map<std::string, std::string>& overrides()
{
  static std::map<std::string, std::string> m;
  return m;
}

void ocudu::rfsim_config_set(const char *name, const char *value) { overrides()[name] = value; }

configmodule_interface_t *rfsim_config_get_if(void)
{
  static configmodule_interface_t cfg = {0};
  return &cfg;
}

int config_paramidx_fromname(const paramdef_t *params, int numparams, const char *name)
{
  for (int i = 0; i < numparams; ++i) {
    if (strcmp(params[i].optname, name) == 0) {
      return i;
    }
  }
  fprintf(stderr, "rfsim: unknown parameter '%s'\n", name);
  abort();
}

paramdef_t *config_get_paramdef_from_name(paramdef_t *params, int numparams, const char *name)
{
  return &params[config_paramidx_fromname(params, numparams, name)];
}

int config_get(configmodule_interface_t *cfg, paramdef_t *params, int numparams, const char *prefix)
{
  (void)cfg;
  (void)prefix;
  for (int i = 0; i < numparams; ++i) {
    paramdef_t *p   = &params[i];
    auto        ovr = overrides().find(p->optname);
    const char *val = (ovr == overrides().end()) ? nullptr : ovr->second.c_str();

    switch (p->type) {
      case TYPE_STRING:

        if (val == nullptr && p->defstrval == nullptr) {
          p->strptr = nullptr;
          break;
        }
        p->strptr  = new char *;
        *p->strptr = strdup(val ? val : p->defstrval);
        break;
      case TYPE_UINT16:
        p->u16ptr  = new uint16_t(val ? strtol(val, nullptr, 0) : p->defuintval);
        break;
      case TYPE_INT:
        p->iptr    = new int32_t(val ? strtol(val, nullptr, 0) : p->defintval);
        break;
      case TYPE_UINT64:
        p->u64ptr  = new uint64_t(val ? strtoull(val, nullptr, 0) : p->defint64val);
        break;
      case TYPE_DOUBLE:
        p->dblptr  = new double(val ? strtod(val, nullptr) : p->defdblval);
        break;
      case TYPE_STRINGLIST:
        p->strlistptr = nullptr;
        p->numelt     = 0;
        break;
      default:
        fprintf(stderr, "rfsim: parameter '%s' has unhandled type %d\n", p->optname, p->type);
        abort();
    }
  }
  return numparams;
}

int config_getlist(configmodule_interface_t *, paramlist_def_t *, paramdef_t *, int, const char *)
{
  return -1;
}

static void no_channel_model(const char *fn)
{
  fprintf(stderr, "rfsim: %s: built without channel modelling\n", fn);
  abort();
}
void            init_channelmod(void)                                 { no_channel_model(__func__); }
int             load_channellist(uint8_t, uint8_t, double, double, double) { no_channel_model(__func__); return 0; }
channel_desc_t *find_channel_desc_fromname(const char *)              { return nullptr; }
int             random_channel(channel_desc_t *, uint8_t)             { no_channel_model(__func__); return 0; }
void            set_channeldesc_owner(channel_desc_t *, int)          { no_channel_model(__func__); }
void            set_channeldesc_direction(channel_desc_t *, int)      { no_channel_model(__func__); }
double          gaussZiggurat(double, double)                         { no_channel_model(__func__); return 0; }
extern "C" {
void rxAddInput(c16_t **, cf_t *, int, channel_desc_t *, int)  { no_channel_model(__func__); }
void update_channel_model(channel_desc_t *, int, uint64_t)      { no_channel_model(__func__); }
}
double          get_noise_power_dBFS(void)                            { return INVALID_DBFS_VALUE; }
void            randominit(void)                                      {}
void            set_taus_seed(unsigned int)                           {}

int32_t signal_energy(int32_t *, uint32_t) { return 0; }
