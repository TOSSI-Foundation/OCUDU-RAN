// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "rfsim_shim.h"
#include "PHY/TOOLS/tools_defs.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>
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
      case TYPE_STRINGLIST: {
        p->strlistptr = nullptr;
        p->numelt     = 0;
        if (val == nullptr) {
          break;
        }
        std::vector<std::string> items;
        std::string_view         rest(val);
        while (!rest.empty()) {
          size_t           sep  = rest.find(':');
          std::string_view item = rest.substr(0, sep);
          if (!item.empty()) {
            items.emplace_back(item);
          }
          if (sep == std::string_view::npos) {
            break;
          }
          rest.remove_prefix(sep + 1);
        }
        if (items.empty()) {
          break;
        }
        p->strlistptr = new char*[items.size()];
        for (size_t item_idx = 0; item_idx != items.size(); ++item_idx) {
          p->strlistptr[item_idx] = strdup(items[item_idx].c_str());
        }
        p->numelt = items.size();
        break;
      }
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

namespace {

struct ntn_link_state {
  std::atomic<double>   rx_delay_us{0.0};
  std::atomic<double>   drift_us_per_s{0.0};
  std::atomic<uint64_t> base_ts{0};
  std::atomic<bool>     base_ts_valid{false};
  std::atomic<double>   doppler_hz{0.0};
  std::atomic<bool>     active{false};
  std::atomic<bool>     link_up{true};
};

ntn_link_state& ntn_link()
{
  static ntn_link_state state;
  return state;
}

double& channel_sampling_rate()
{
  static double sample_rate = 0.0;
  return sample_rate;
}

int& channel_nb_tx()
{
  static int nb_tx = 1;
  return nb_tx;
}

std::map<std::string, channel_desc_t*>& channel_models()
{
  static std::map<std::string, channel_desc_t*> models;
  return models;
}

channel_desc_t* make_model(const std::string& name)
{
  auto* desc           = new channel_desc_t{};
  desc->model_name     = strdup(name.c_str());
  desc->channel_length = 1;
  desc->nb_tx          = channel_nb_tx();
  desc->sampling_rate  = channel_sampling_rate();
  return desc;
}

} // namespace

void ocudu::rfsim_set_ntn_channel(double rx_delay_us, double drift_us_per_s, bool link_up)
{
  ntn_link().link_up.store(link_up, std::memory_order_relaxed);
  ntn_link().rx_delay_us.store(rx_delay_us, std::memory_order_relaxed);
  ntn_link().drift_us_per_s.store(drift_us_per_s, std::memory_order_relaxed);
  ntn_link().base_ts_valid.store(false, std::memory_order_relaxed);
  ntn_link().active.store(true, std::memory_order_release);
}

void ocudu::rfsim_set_ntn_doppler(double doppler_hz)
{
  ntn_link().doppler_hz.store(doppler_hz, std::memory_order_relaxed);
}

void init_channelmod(void) {}

int load_channellist(uint8_t nb_tx, uint8_t nb_rx, double sr, double cf, double bw)
{
  (void)nb_rx;
  (void)cf;
  (void)bw;
  channel_sampling_rate() = sr;
  channel_nb_tx()         = (nb_tx > 0) ? nb_tx : 1;
  rfsim_log(OAILOG_INFO, 0, "NTN channel model enabled, sample rate %.3f Msps\n", sr / 1e6);
  return 0;
}

channel_desc_t* find_channel_desc_fromname(const char* name)
{
  if (name == nullptr) {
    return nullptr;
  }
  auto& models = channel_models();
  auto  it     = models.find(name);
  if (it != models.end()) {
    return it->second;
  }
  channel_desc_t* desc = make_model(name);
  models.emplace(name, desc);
  return desc;
}

channel_desc_t* new_channel_desc_scm(uint8_t nb_tx,
                                     uint8_t,
                                     SCM_t,
                                     double sr,
                                     double,
                                     double,
                                     double,
                                     double,
                                     int,
                                     double,
                                     uint64_t off,
                                     double,
                                     float)
{
  channel_desc_t* desc = make_model("");
  desc->nb_tx          = (nb_tx > 0) ? nb_tx : 1;
  desc->sampling_rate  = (sr > 0.0) ? sr : channel_sampling_rate();
  desc->channel_offset = off;
  return desc;
}

void free_channel_desc_scm(channel_desc_t* desc)
{
  if (desc == nullptr) {
    return;
  }
  for (const auto& entry : channel_models()) {
    if (entry.second == desc) {
      return;
    }
  }
  free(desc->model_name);
  delete desc;
}

void set_channeldesc_name(channel_desc_t* desc, const char* name)
{
  if (desc == nullptr || name == nullptr) {
    return;
  }
  free(desc->model_name);
  desc->model_name = strdup(name);
}

void set_channeldesc_direction(channel_desc_t* desc, int is_uplink)
{
  if (desc != nullptr) {
    desc->is_uplink = (is_uplink != 0);
  }
}

int random_channel(channel_desc_t*, uint8_t)
{
  return 0;
}
void set_channeldesc_owner(channel_desc_t*, int) {}
int  modelid_fromstrtype(const char*)
{
  return 0;
}
int modelid_fromstrmodeltype(char*)
{
  return 0;
}

extern "C" {

void update_channel_model(channel_desc_t* desc, int nbSamples, uint64_t TS)
{
  (void)nbSamples;
  if (desc == nullptr || !ntn_link().active.load(std::memory_order_acquire)) {
    return;
  }
  const double sample_rate = (desc->sampling_rate > 0.0) ? desc->sampling_rate : channel_sampling_rate();
  if (sample_rate <= 0.0) {
    return;
  }

  if (!ntn_link().base_ts_valid.load(std::memory_order_relaxed)) {
    ntn_link().base_ts.store(TS, std::memory_order_relaxed);
    ntn_link().base_ts_valid.store(true, std::memory_order_relaxed);
  }
  const uint64_t base_ts   = ntn_link().base_ts.load(std::memory_order_relaxed);
  const double   elapsed_s = (TS > base_ts) ? static_cast<double>(TS - base_ts) / sample_rate : 0.0;
  const double   drift     = ntn_link().drift_us_per_s.load(std::memory_order_relaxed);
  const double   delay_us  = ntn_link().rx_delay_us.load(std::memory_order_relaxed) + drift * elapsed_s;
  desc->channel_offset     = static_cast<uint64_t>(std::llround(std::max(delay_us, 0.0) * 1e-6 * sample_rate));

  const double doppler    = ntn_link().doppler_hz.load(std::memory_order_relaxed);
  desc->doppler_phase_inc = 2.0 * M_PI * doppler / sample_rate;
}

void rxAddInput(c16_t** input_sig, cf_t* after_channel_sig, int rxAnt, channel_desc_t* channelDesc, int nbSamples)
{
  if (channelDesc == nullptr || input_sig == nullptr || after_channel_sig == nullptr) {
    return;
  }
  if (ntn_link().active.load(std::memory_order_acquire) && !ntn_link().link_up.load(std::memory_order_relaxed)) {
    return;
  }

  const int ant   = (rxAnt >= 0 && rxAnt < RFSIM_SHIM_MAX_RX_ANT) ? rxAnt : 0;
  const int nb_tx = (channelDesc->nb_tx > 0) ? channelDesc->nb_tx : 1;

  double       phase     = channelDesc->doppler_phase[ant];
  const double phase_inc = channelDesc->doppler_phase_inc;

  for (int i = 0; i != nbSamples; ++i) {
    const double cs = std::cos(phase);
    const double sn = std::sin(phase);
    for (int tx = 0; tx != nb_tx; ++tx) {
      const c16_t in = input_sig[tx][i];
      after_channel_sig[i].r += static_cast<float>(in.r * cs - in.i * sn);
      after_channel_sig[i].i += static_cast<float>(in.r * sn + in.i * cs);
    }
    phase += phase_inc;
  }

  channelDesc->doppler_phase[ant] = std::fmod(phase, 2.0 * M_PI);
}
}

double get_noise_power_dBFS(void)
{
  return INVALID_DBFS_VALUE;
}
double gaussZiggurat(double, double)
{
  return 0.0;
}
void randominit(void) {}
void set_taus_seed(unsigned int) {}

int32_t signal_energy(int32_t*, uint32_t)
{
  return 0;
}
