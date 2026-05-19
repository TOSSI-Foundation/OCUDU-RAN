#include "fapi_serializer_p5.h"
#include "ocudu/ran/dmrs/dmrs.h"
#include "ocudu/ran/duplex_mode.h"
#include "ocudu/ran/prach/restricted_set_config.h"

using namespace ocudu;
using namespace ocudu::fapi_serial;


void fapi_serial::serialize(buffer_writer& w, const fapi::param_request&) { /* empty struct */ }
void fapi_serial::deserialize(buffer_reader& r, fapi::param_request&) { /* empty struct */ }

void fapi_serial::serialize(buffer_writer& w, const fapi::param_response& msg) { serialize_enum_u8(w, msg.error_code); }
void fapi_serial::deserialize(buffer_reader& r, fapi::param_response& msg) { deserialize_enum_u8(r, msg.error_code); }

void fapi_serial::serialize(buffer_writer& w, const fapi::config_response& msg) { serialize_enum_u8(w, msg.error_code); }
void fapi_serial::deserialize(buffer_reader& r, fapi::config_response& msg) { deserialize_enum_u8(r, msg.error_code); }

void fapi_serial::serialize(buffer_writer& w, const fapi::start_request&) { /* empty struct */ }
void fapi_serial::deserialize(buffer_reader& r, fapi::start_request&) { /* empty struct */ }

void fapi_serial::serialize(buffer_writer& w, const fapi::stop_request&) { /* empty struct */ }
void fapi_serial::deserialize(buffer_reader& r, fapi::stop_request&) { /* empty struct */ }

void fapi_serial::serialize(buffer_writer& w, const fapi::stop_indication&) { /* empty struct */ }
void fapi_serial::deserialize(buffer_reader& r, fapi::stop_indication&) { /* empty struct */ }


void fapi_serial::serialize(buffer_writer& w, const fapi::carrier_config& cfg)
{
  w.write_u16(cfg.dl_bandwidth);
  w.write_u32(cfg.dl_f_ref_arfcn);
  w.write_array_u16<5>(cfg.dl_k0);
  w.write_array_u16<5>(cfg.dl_grid_size);
  w.write_u16(cfg.num_tx_ant);
  w.write_u16(cfg.ul_bandwidth);
  w.write_u32(cfg.ul_f_ref_arfcn);
  w.write_array_u16<5>(cfg.ul_k0);
  w.write_array_u16<5>(cfg.ul_grid_size);
  w.write_u16(cfg.num_rx_ant);
  w.write_u8(cfg.freq_shift_7p5kHz);
  w.write_u8(cfg.power_profile);
  w.write_u8(cfg.power_offset_rs_index);
  serialize_enum_u8(w, cfg.dmrs_typeA_pos);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::carrier_config& cfg)
{
  cfg.dl_bandwidth = r.read_u16();
  cfg.dl_f_ref_arfcn = r.read_u32();
  r.read_array_u16<5>(cfg.dl_k0);
  r.read_array_u16<5>(cfg.dl_grid_size);
  cfg.num_tx_ant = r.read_u16();
  cfg.ul_bandwidth = r.read_u16();
  cfg.ul_f_ref_arfcn = r.read_u32();
  r.read_array_u16<5>(cfg.ul_k0);
  r.read_array_u16<5>(cfg.ul_grid_size);
  cfg.num_rx_ant = r.read_u16();
  cfg.freq_shift_7p5kHz = r.read_u8();
  cfg.power_profile = r.read_u8();
  cfg.power_offset_rs_index = r.read_u8();
  deserialize_enum_u8(r, cfg.dmrs_typeA_pos);
}


static void serialize_rach_config_generic(buffer_writer& w, const rach_config_generic& cfg)
{
  w.write_u8(cfg.prach_config_index);
  w.write_u32(cfg.ra_resp_window);
  w.write_u32(cfg.msg1_fdm);
  w.write_u32(cfg.msg1_frequency_start);
  w.write_u16(cfg.zero_correlation_zone_config);
  w.write_i32(static_cast<int32_t>(cfg.preamble_rx_target_pw.value()));
  w.write_u8(cfg.preamble_trans_max);
  w.write_u8(cfg.power_ramping_step_db);
}

static void deserialize_rach_config_generic(buffer_reader& r, rach_config_generic& cfg)
{
  cfg.prach_config_index          = r.read_u8();
  cfg.ra_resp_window              = r.read_u32();
  cfg.msg1_fdm                    = r.read_u32();
  cfg.msg1_frequency_start        = r.read_u32();
  cfg.zero_correlation_zone_config = r.read_u16();
  cfg.preamble_rx_target_pw       = r.read_i32();
  cfg.preamble_trans_max          = r.read_u8();
  cfg.power_ramping_step_db       = r.read_u8();
}


static void serialize_ra_prioritization(buffer_writer& w, const ra_prioritization& prio)
{
  serialize_enum_u8(w, prio.pwr_ramp_step_hi_prio);
  if (prio.scaling_bi.has_value()) {
    w.write_u8(1);
    serialize_enum_u8(w, prio.scaling_bi.value());
  } else {
    w.write_u8(0);
  }
}

static void deserialize_ra_prioritization(buffer_reader& r, ra_prioritization& prio)
{
  deserialize_enum_u8(r, prio.pwr_ramp_step_hi_prio);
  uint8_t has_scaling = r.read_u8();
  if (has_scaling) {
    ra_prioritization::scaling_factor_bi val;
    deserialize_enum_u8(r, val);
    prio.scaling_bi = val;
  } else {
    prio.scaling_bi = std::nullopt;
  }
}


static void serialize_ra_prio_slice(buffer_writer& w, const ra_prioritization_slice_info& info)
{
  w.write_u32(static_cast<uint32_t>(info.nsag_id_list.size()));
  if (!info.nsag_id_list.empty()) {
    w.write_bytes(info.nsag_id_list.data(), static_cast<uint32_t>(info.nsag_id_list.size()));
  }
  serialize_ra_prioritization(w, info.prio);
}

static void deserialize_ra_prio_slice(buffer_reader& r, ra_prioritization_slice_info& info)
{
  uint32_t n = r.read_u32();
  info.nsag_id_list.resize(n);
  if (n > 0) {
    r.read_bytes(info.nsag_id_list.data(), n);
  }
  deserialize_ra_prioritization(r, info.prio);
}


void fapi_serial::serialize(buffer_writer& w, const rach_config_common& cfg)
{
  serialize_rach_config_generic(w, cfg.rach_cfg_generic);
  w.write_u32(cfg.total_nof_ra_preambles);
  serialize(w, cfg.ra_con_res_timer);
  w.write_bool(cfg.is_prach_root_seq_index_l839);
  w.write_u32(cfg.prach_root_seq_index);
  serialize(w, cfg.msg1_scs);
  serialize_enum_u8(w, cfg.restricted_set);
  w.write_bool(cfg.msg3_transform_precoder);
  w.write_u8(static_cast<uint8_t>(cfg.nof_ssb_per_ro));
  w.write_u8(cfg.nof_cb_preambles_per_ssb);
  w.write_u32(static_cast<uint32_t>(cfg.ra_prio_slice_info_list.size()));
  for (const auto& info : cfg.ra_prio_slice_info_list) {
    serialize_ra_prio_slice(w, info);
  }
}

void fapi_serial::deserialize(buffer_reader& r, rach_config_common& cfg)
{
  deserialize_rach_config_generic(r, cfg.rach_cfg_generic);
  cfg.total_nof_ra_preambles = r.read_u32();
  deserialize(r, cfg.ra_con_res_timer);
  cfg.is_prach_root_seq_index_l839 = r.read_bool();
  cfg.prach_root_seq_index = r.read_u32();
  deserialize(r, cfg.msg1_scs);
  deserialize_enum_u8(r, cfg.restricted_set);
  cfg.msg3_transform_precoder = r.read_bool();
  cfg.nof_ssb_per_ro = static_cast<decltype(cfg.nof_ssb_per_ro)>(r.read_u8());
  cfg.nof_cb_preambles_per_ssb = r.read_u8();
  uint32_t n_slices = r.read_u32();
  cfg.ra_prio_slice_info_list.clear();
  for (uint32_t i = 0; i < n_slices; ++i) {
    ra_prioritization_slice_info info;
    deserialize_ra_prio_slice(r, info);
    cfg.ra_prio_slice_info_list.push_back(std::move(info));
  }
}


void fapi_serial::serialize(buffer_writer& w, const ssb_configuration& cfg)
{
  serialize(w, cfg.scs);
  w.write_u16(cfg.offset_to_point_A.value());
  serialize_enum_u16(w, cfg.ssb_period);
  w.write_u8(cfg.k_ssb.value());
  serialize(w, static_cast<const bounded_bitset<NOF_SSB_BEAMS, true>&>(cfg.ssb_bitmap));
  w.write_bytes(cfg.beam_ids.data(), NOF_SSB_BEAMS);
  serialize_enum_u8(w, cfg.pss_to_sss_epre);
  w.write_i32(cfg.ssb_block_power);
}

void fapi_serial::deserialize(buffer_reader& r, ssb_configuration& cfg)
{
  deserialize(r, cfg.scs);
  cfg.offset_to_point_A = ssb_offset_to_pointA(r.read_u16());
  deserialize_enum_u16(r, cfg.ssb_period);
  cfg.k_ssb = ssb_subcarrier_offset(r.read_u8());
  bounded_bitset<NOF_SSB_BEAMS, true> tmp_bs;
  deserialize(r, tmp_bs);
  cfg.ssb_bitmap = ssb_bitmap_t();
  cfg.ssb_bitmap.resize(tmp_bs.size());
  for (unsigned i = 0; i < tmp_bs.size(); ++i) {
    if (tmp_bs.test(i)) {
      cfg.ssb_bitmap.set(i);
    }
  }
  r.read_bytes(cfg.beam_ids.data(), NOF_SSB_BEAMS);
  deserialize_enum_u8(r, cfg.pss_to_sss_epre);
  cfg.ssb_block_power = r.read_i32();
}


static void serialize_tdd_pattern(buffer_writer& w, const tdd_ul_dl_pattern& p)
{
  w.write_u32(p.dl_ul_tx_period_nof_slots);
  w.write_u32(p.nof_dl_slots);
  w.write_u32(p.nof_dl_symbols);
  w.write_u32(p.nof_ul_slots);
  w.write_u32(p.nof_ul_symbols);
}

static void deserialize_tdd_pattern(buffer_reader& r, tdd_ul_dl_pattern& p)
{
  p.dl_ul_tx_period_nof_slots = r.read_u32();
  p.nof_dl_slots              = r.read_u32();
  p.nof_dl_symbols            = r.read_u32();
  p.nof_ul_slots              = r.read_u32();
  p.nof_ul_symbols            = r.read_u32();
}


void fapi_serial::serialize(buffer_writer& w, const fapi::cell_configuration& cfg)
{
  serialize(w, cfg.scs_common);
  serialize(w, cfg.cp);
  w.write_u16(cfg.pci);
  serialize_enum_u8(w, cfg.duplex);
  serialize(w, cfg.carrier_cfg);
  serialize(w, cfg.prach_cfg);
  serialize(w, cfg.ssb_cfg);

  if (cfg.tdd_ul_dl_cfg_common.has_value()) {
    w.write_u8(1);
    const auto& tdd = cfg.tdd_ul_dl_cfg_common.value();
    serialize(w, tdd.ref_scs);
    serialize_tdd_pattern(w, tdd.pattern1);
    if (tdd.pattern2.has_value()) {
      w.write_u8(1);
      serialize_tdd_pattern(w, tdd.pattern2.value());
    } else {
      w.write_u8(0);
    }
  } else {
    w.write_u8(0);
  }

  w.write_u8(0);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::cell_configuration& cfg)
{
  deserialize(r, cfg.scs_common);
  deserialize(r, cfg.cp);
  cfg.pci = r.read_u16();
  deserialize_enum_u8(r, cfg.duplex);
  deserialize(r, cfg.carrier_cfg);
  deserialize(r, cfg.prach_cfg);
  deserialize(r, cfg.ssb_cfg);

  uint8_t has_tdd = r.read_u8();
  if (has_tdd) {
    tdd_ul_dl_config_common tdd;
    deserialize(r, tdd.ref_scs);
    deserialize_tdd_pattern(r, tdd.pattern1);
    uint8_t has_p2 = r.read_u8();
    if (has_p2) {
      tdd_ul_dl_pattern p2;
      deserialize_tdd_pattern(r, p2);
      tdd.pattern2 = p2;
    }
    cfg.tdd_ul_dl_cfg_common = tdd;
  } else {
    cfg.tdd_ul_dl_cfg_common = std::nullopt;
  }

  r.read_u8();
  cfg.vendor_cfg = std::any{}; // empty
}


void fapi_serial::serialize(buffer_writer& w, const fapi::config_request& msg)
{
  serialize(w, msg.cell_cfg);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::config_request& msg)
{
  deserialize(r, msg.cell_cfg);
}
