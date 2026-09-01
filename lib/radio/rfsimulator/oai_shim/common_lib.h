#pragma once
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"

typedef int64_t openair0_timestamp_t;
typedef struct openair0_device_t openair0_device_t;

typedef struct {
  uint32_t size;
  uint32_t nbAnt;
  uint64_t timestamp;
  uint32_t option_value;
  uint32_t option_flag;
  uint64_t beam_map;
} samplesBlockHeader_t;

typedef enum { MAX_CARDS = 8 } openair0_limits_e;
typedef enum { RFSIMULATOR = 100 } dev_type_t;
typedef enum { RAU_HOST = 3 } host_type_t;
enum { TX_BURST_START = 1, TX_BURST_END = 2, TX_BURST_START_AND_END = 3 };

typedef struct {
  int    ru_id;
  int    tx_num_channels;
  int    rx_num_channels;
  double sample_rate;
  double rx_freq[MAX_CARDS];
  double tx_freq[MAX_CARDS];
  double rx_gain[MAX_CARDS];
  double tx_gain[MAX_CARDS];
  double tx_bw;
  double rx_bw;
  int    command_line_sample_advance;
} openair0_config_t;

struct openair0_device_t {
  dev_type_t  type;
  host_type_t host_type;
  openair0_config_t *openair0_cfg;
  void *priv;
  int  (*trx_start_func)(openair0_device_t *device);
  void (*trx_end_func)(openair0_device_t *device);
  int  (*trx_stop_func)(openair0_device_t *device);
  int  (*trx_get_stats_func)(openair0_device_t *device);
  int  (*trx_reset_stats_func)(openair0_device_t *device);
  int  (*trx_set_freq_func)(openair0_device_t *device, openair0_config_t *cfg);
  int  (*trx_set_gains_func)(openair0_device_t *device, openair0_config_t *cfg);
  int  (*trx_write_init)(openair0_device_t *device);
  int  (*trx_write_func)(openair0_device_t *device, openair0_timestamp_t ts, void **buff, int nsamps, int cc, int flags);
  int  (*trx_read_func)(openair0_device_t *device, openair0_timestamp_t *ts, void **buff, int nsamps, int nbAnt);
  int  (*trx_write_beams_func)(openair0_device_t *device, openair0_timestamp_t ts, void ***buff, int nsamps, int cc, int nb, int flags);
  int  (*trx_read_beams_func)(openair0_device_t *device, openair0_timestamp_t *ts, void ***buff, int nsamps, int nbAnt, int nb);
  int  (*trx_set_beams)(openair0_device_t *device, uint64_t beam_map, openair0_timestamp_t ts);
  int  (*trx_set_beams2)(openair0_device_t *device, int *beams, int num_beams, openair0_timestamp_t ts);
};

#ifdef __cplusplus
extern "C"
#endif
int device_init(openair0_device_t *device, openair0_config_t *openair0_cfg);
