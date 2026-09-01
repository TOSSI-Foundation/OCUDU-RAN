#pragma once
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"

#define INVALID_DBFS_VALUE (-1000.0)
#define RFSIMU_MODULEID 1
#define CORR_LEVEL_LOW  0
#define CORR_LEVEL_MEDIUM 1
#define CORR_LEVEL_HIGH 2
typedef int SCM_t;

typedef struct channel_desc_s {
  char    *model_name;
  uint64_t channel_offset;
  uint32_t channel_length;
  int64_t  start_TS;
} channel_desc_t;

#ifdef __cplusplus
extern "C" {
#endif
void            init_channelmod(void);
int             load_channellist(uint8_t nb_tx, uint8_t nb_rx, double sr, double cf, double bw);
channel_desc_t *find_channel_desc_fromname(const char *name);
int             random_channel(channel_desc_t *desc, uint8_t abstraction_flag);
void            free_channel_desc_scm(channel_desc_t *desc);
void            set_channeldesc_owner(channel_desc_t *desc, int owner);
void            set_channeldesc_direction(channel_desc_t *desc, int is_uplink);
double          get_noise_power_dBFS(void);
double          gaussZiggurat(double mean, double variance);
void            randominit(void);
int             modelid_fromstrtype(const char *modeltype);
void            set_channeldesc_name(channel_desc_t *desc, const char *name);
channel_desc_t *new_channel_desc_scm(uint8_t nb_tx, uint8_t nb_rx, SCM_t model, double sr, double cf, double bw,
                                     double dd, double pd, int corr, double fs, uint64_t off, double ploss, float noise);
void            set_taus_seed(unsigned int seed);
int             modelid_fromstrmodeltype(char *modeltype);
#ifdef __cplusplus
}
#endif
