#include "fapi_serializer_p7_ul.h"
#include "ocudu/ran/pusch/pusch_context.h"

using namespace ocudu;
using namespace ocudu::fapi_serial;


static void serialize_uci_correspondence(buffer_writer& w, const fapi::uci_part1_to_part2_correspondence_v3& corr)
{
  uint16_t count = static_cast<uint16_t>(corr.part2.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    const auto& info = corr.part2[i];
    w.write_u16(info.priority);
    serialize_static_vector_u16(w, info.param_offsets);
    serialize_static_vector_u8(w, info.param_sizes);
    w.write_u16(info.part2_size_map_index);
    serialize_enum_u8(w, info.part2_size_map_scope);
  }
}

static void deserialize_uci_correspondence(buffer_reader& r, fapi::uci_part1_to_part2_correspondence_v3& corr)
{
  uint16_t count = r.read_u16();
  corr.part2.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::uci_part1_to_part2_correspondence_v3::part2_info info{};
    info.priority = r.read_u16();
    deserialize_static_vector_u16(r, info.param_offsets);
    deserialize_static_vector_u8(r, info.param_sizes);
    info.part2_size_map_index = r.read_u16();
    deserialize_enum_u8(r, info.part2_size_map_scope);
    corr.part2.push_back(std::move(info));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_prach_pdu& pdu)
{
  w.write_u8(pdu.num_prach_ocas);
  serialize_enum_u8(w, pdu.prach_format);
  w.write_u8(pdu.index_fd_ra);
  w.write_u8(pdu.prach_start_symbol);
  w.write_u16(pdu.num_cs);
  w.write_u32(pdu.handle);
  w.write_u8(pdu.num_fd_ra);
  serialize(w, pdu.preambles);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_prach_pdu& pdu)
{
  pdu.num_prach_ocas    = r.read_u8();
  deserialize_enum_u8(r, pdu.prach_format);
  pdu.index_fd_ra       = r.read_u8();
  pdu.prach_start_symbol = r.read_u8();
  pdu.num_cs            = r.read_u16();
  pdu.handle            = r.read_u32();
  pdu.num_fd_ra         = r.read_u8();
  deserialize(r, pdu.preambles);
}


static void serialize_pusch_data(buffer_writer& w, const fapi::ul_pusch_data& d)
{
  w.write_u8(d.rv_index);
  w.write_u8(d.harq_process_id);
  w.write_bool(d.new_data);
  serialize(w, d.tb_size);
  w.write_u16(d.num_cb);
  serialize_static_vector_u8(w, d.cb_present_and_position);
}

static void deserialize_pusch_data(buffer_reader& r, fapi::ul_pusch_data& d)
{
  d.rv_index         = r.read_u8();
  d.harq_process_id  = r.read_u8();
  d.new_data         = r.read_bool();
  deserialize(r, d.tb_size);
  d.num_cb           = r.read_u16();
  deserialize_static_vector_u8(r, d.cb_present_and_position);
}

static void serialize_pusch_uci(buffer_writer& w, const fapi::ul_pusch_uci& u)
{
  w.write_u16(u.harq_ack_bit_length);
  w.write_u16(u.csi_part1_bit_length);
  w.write_u16(u.flags_csi_part2);
  serialize_enum_u8(w, u.alpha_scaling);
  w.write_u8(u.beta_offset_harq_ack);
  w.write_u8(u.beta_offset_csi1);
  w.write_u8(u.beta_offset_csi2);
}

static void deserialize_pusch_uci(buffer_reader& r, fapi::ul_pusch_uci& u)
{
  u.harq_ack_bit_length  = r.read_u16();
  u.csi_part1_bit_length = r.read_u16();
  u.flags_csi_part2      = r.read_u16();
  deserialize_enum_u8(r, u.alpha_scaling);
  u.beta_offset_harq_ack = r.read_u8();
  u.beta_offset_csi1     = r.read_u8();
  u.beta_offset_csi2     = r.read_u8();
}

static void serialize_pusch_ptrs(buffer_writer& w, const fapi::ul_pusch_ptrs& p)
{
  uint16_t nof_ports = static_cast<uint16_t>(p.port_info.size());
  w.write_u16(nof_ports);
  for (uint16_t i = 0; i < nof_ports; ++i) {
    w.write_u16(p.port_info[i].ptrs_port_index);
    w.write_u8(p.port_info[i].ptrs_dmrs_port);
    w.write_u8(p.port_info[i].ptrs_re_offset);
  }
  w.write_u8(p.ptrs_time_density);
  w.write_u8(p.ptrs_freq_density);
  serialize_enum_u8(w, p.ul_ptrs_power);
}

static void deserialize_pusch_ptrs(buffer_reader& r, fapi::ul_pusch_ptrs& p)
{
  uint16_t nof_ports = r.read_u16();
  p.port_info.clear();
  for (uint16_t i = 0; i < nof_ports; ++i) {
    fapi::ul_pusch_ptrs::ptrs_port_info info{};
    info.ptrs_port_index = r.read_u16();
    info.ptrs_dmrs_port  = r.read_u8();
    info.ptrs_re_offset  = r.read_u8();
    p.port_info.push_back(info);
  }
  p.ptrs_time_density = r.read_u8();
  p.ptrs_freq_density = r.read_u8();
  deserialize_enum_u8(r, p.ul_ptrs_power);
}

static void serialize_pusch_dfts_ofdm(buffer_writer& w, const fapi::ul_pusch_dfts_ofdm& d)
{
  w.write_u8(d.low_papr_group_number);
  w.write_u16(d.low_papr_sequence_number);
  w.write_u8(d.ul_ptrs_sample_density);
  w.write_u8(d.ul_ptrs_time_density_transform_precoding);
}

static void deserialize_pusch_dfts_ofdm(buffer_reader& r, fapi::ul_pusch_dfts_ofdm& d)
{
  d.low_papr_group_number    = r.read_u8();
  d.low_papr_sequence_number = r.read_u16();
  d.ul_ptrs_sample_density   = r.read_u8();
  d.ul_ptrs_time_density_transform_precoding = r.read_u8();
}

static void serialize_pusch_maintenance_v3(buffer_writer& w, const fapi::ul_pusch_maintenance_v3& m)
{
  w.write_u8(m.pusch_trans_type);
  w.write_u16(m.delta_bwp0_start_from_active_bwp);
  w.write_u16(m.initial_ul_bwp_size);
  w.write_u8(m.group_or_sequence_hopping);
  w.write_u16(m.pusch_second_hop_prb);
  serialize_enum_u8(w, m.ldpc_base_graph);
  serialize(w, m.tb_size_lbrm_bytes);
}

static void deserialize_pusch_maintenance_v3(buffer_reader& r, fapi::ul_pusch_maintenance_v3& m)
{
  m.pusch_trans_type                  = r.read_u8();
  m.delta_bwp0_start_from_active_bwp  = r.read_u16();
  m.initial_ul_bwp_size               = r.read_u16();
  m.group_or_sequence_hopping         = r.read_u8();
  m.pusch_second_hop_prb              = r.read_u16();
  deserialize_enum_u8(r, m.ldpc_base_graph);
  deserialize(r, m.tb_size_lbrm_bytes);
}

static void serialize_pusch_params_v4(buffer_writer& w, const fapi::ul_pusch_params_v4& p)
{
  w.write_bool(p.cb_crc_status_request);
  w.write_u32(p.srs_tx_ports);
  w.write_u8(p.ul_tpmi_index);
  w.write_u8(p.num_ul_spatial_streams_ports);
  w.write_bytes(p.ul_spatial_stream_ports.data(), fapi::ul_pusch_params_v4::MAX_NUM_SPATIAL_STREAMS);
}

static void deserialize_pusch_params_v4(buffer_reader& r, fapi::ul_pusch_params_v4& p)
{
  p.cb_crc_status_request        = r.read_bool();
  p.srs_tx_ports                 = r.read_u32();
  p.ul_tpmi_index                = r.read_u8();
  p.num_ul_spatial_streams_ports = r.read_u8();
  r.read_bytes(p.ul_spatial_stream_ports.data(), fapi::ul_pusch_params_v4::MAX_NUM_SPATIAL_STREAMS);
}

void fapi_serial::serialize(buffer_writer& w, const fapi::ul_pusch_pdu& pdu)
{
  serialize(w, pdu.pdu_bitmap);
  serialize(w, pdu.rnti);
  w.write_u32(pdu.handle);
  serialize(w, pdu.bwp);
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  w.write_u16(pdu.target_code_rate);
  serialize_enum_u8(w, pdu.qam_mod_order);
  w.write_u8(pdu.mcs_index);
  serialize_enum_u8(w, pdu.mcs_table);
  w.write_bool(pdu.transform_precoding);
  w.write_u16(pdu.nid_pusch);
  w.write_u8(pdu.num_layers);
  w.write_u16(pdu.ul_dmrs_symb_pos);
  serialize_enum_u8(w, pdu.dmrs_type);
  w.write_u16(pdu.pusch_dmrs_scrambling_id);
  w.write_u16(pdu.pusch_dmrs_scrambling_id_complement);
  serialize_enum_u8(w, pdu.low_papr_dmrs);
  w.write_u16(pdu.pusch_dmrs_identity);
  w.write_u8(pdu.nscid);
  w.write_u8(pdu.num_dmrs_cdm_grps_no_data);
  w.write_u16(pdu.dmrs_ports);
  serialize_enum_u8(w, pdu.resource_alloc);
  w.write_bytes(pdu.rb_bitmap.data(), fapi::ul_pusch_pdu::RB_BITMAP_SIZE_IN_BYTES);
  serialize(w, pdu.vrbs);
  serialize_enum_u8(w, pdu.vrb_to_prb_mapping);
  w.write_bool(pdu.intra_slot_frequency_hopping);
  w.write_u16(pdu.tx_direct_current_location);
  w.write_bool(pdu.uplink_frequency_shift_7p5kHz);
  serialize(w, pdu.symbols);

  serialize_pusch_data(w, pdu.pusch_data);
  serialize_pusch_uci(w, pdu.pusch_uci);
  serialize_pusch_ptrs(w, pdu.pusch_ptrs);
  serialize_pusch_dfts_ofdm(w, pdu.pusch_ofdm);
  serialize_pusch_maintenance_v3(w, pdu.pusch_maintenance_v3);
  serialize_pusch_params_v4(w, pdu.pusch_params_v4);
  serialize_uci_correspondence(w, pdu.uci_correspondence);

  if (pdu.context.has_value()) {
    w.write_u8(1);
    const auto& ctx = pdu.context.value();
    serialize(w, ctx.get_rnti());
    serialize(w, ctx.get_h_id());
  } else {
    w.write_u8(0);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_pusch_pdu& pdu)
{
  deserialize(r, pdu.pdu_bitmap);
  deserialize(r, pdu.rnti);
  pdu.handle = r.read_u32();
  deserialize(r, pdu.bwp);
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  pdu.target_code_rate = r.read_u16();
  deserialize_enum_u8(r, pdu.qam_mod_order);
  pdu.mcs_index = r.read_u8();
  deserialize_enum_u8(r, pdu.mcs_table);
  pdu.transform_precoding = r.read_bool();
  pdu.nid_pusch  = r.read_u16();
  pdu.num_layers = r.read_u8();
  pdu.ul_dmrs_symb_pos = r.read_u16();
  deserialize_enum_u8(r, pdu.dmrs_type);
  pdu.pusch_dmrs_scrambling_id            = r.read_u16();
  pdu.pusch_dmrs_scrambling_id_complement = r.read_u16();
  deserialize_enum_u8(r, pdu.low_papr_dmrs);
  pdu.pusch_dmrs_identity       = r.read_u16();
  pdu.nscid                     = r.read_u8();
  pdu.num_dmrs_cdm_grps_no_data = r.read_u8();
  pdu.dmrs_ports                = r.read_u16();
  deserialize_enum_u8(r, pdu.resource_alloc);
  r.read_bytes(pdu.rb_bitmap.data(), fapi::ul_pusch_pdu::RB_BITMAP_SIZE_IN_BYTES);
  deserialize(r, pdu.vrbs);
  deserialize_enum_u8(r, pdu.vrb_to_prb_mapping);
  pdu.intra_slot_frequency_hopping  = r.read_bool();
  pdu.tx_direct_current_location    = r.read_u16();
  pdu.uplink_frequency_shift_7p5kHz = r.read_bool();
  deserialize(r, pdu.symbols);

  deserialize_pusch_data(r, pdu.pusch_data);
  deserialize_pusch_uci(r, pdu.pusch_uci);
  deserialize_pusch_ptrs(r, pdu.pusch_ptrs);
  deserialize_pusch_dfts_ofdm(r, pdu.pusch_ofdm);
  deserialize_pusch_maintenance_v3(r, pdu.pusch_maintenance_v3);
  deserialize_pusch_params_v4(r, pdu.pusch_params_v4);
  deserialize_uci_correspondence(r, pdu.uci_correspondence);

  uint8_t has_ctx = r.read_u8();
  if (has_ctx) {
    rnti_t    ctx_rnti;
    harq_id_t ctx_h_id;
    deserialize(r, ctx_rnti);
    deserialize(r, ctx_h_id);
    pdu.context = pusch_context(ctx_rnti, ctx_h_id);
  } else {
    pdu.context = std::nullopt;
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_pucch_pdu& pdu)
{
  serialize(w, pdu.rnti);
  w.write_u32(pdu.handle);
  serialize(w, pdu.bwp);
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  serialize_enum_u8(w, pdu.format_type);
  serialize_enum_u8(w, pdu.multi_slot_tx_indicator);
  w.write_bool(pdu.pi2_bpsk);
  serialize(w, pdu.prbs);
  serialize(w, pdu.symbols);
  w.write_bool(pdu.intra_slot_frequency_hopping);
  w.write_u16(pdu.second_hop_prb);
  serialize_enum_u8(w, pdu.pucch_grp_hopping);
  w.write_u8(pdu.reserved);
  w.write_u16(pdu.nid_pucch_hopping);
  w.write_u16(pdu.initial_cyclic_shift);
  w.write_u16(pdu.nid_pucch_scrambling);
  w.write_u8(pdu.time_domain_occ_index);
  w.write_u8(pdu.pre_dft_occ_idx);
  w.write_u8(pdu.pre_dft_occ_len);
  w.write_bool(pdu.add_dmrs_flag);
  w.write_u16(pdu.nid0_pucch_dmrs_scrambling);
  w.write_u8(pdu.m0_pucch_dmrs_cyclic_shift);
  w.write_u8(pdu.sr_bit_len);
  w.write_u16(pdu.bit_len_harq);
  w.write_u16(pdu.csi_part1_bit_length);
  w.write_u8(pdu.pucch_maintenance_v3.max_code_rate);
  w.write_u8(pdu.pucch_maintenance_v3.ul_bwp_id);
  serialize_uci_correspondence(w, pdu.uci_correspondence);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_pucch_pdu& pdu)
{
  deserialize(r, pdu.rnti);
  pdu.handle = r.read_u32();
  deserialize(r, pdu.bwp);
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  deserialize_enum_u8(r, pdu.format_type);
  deserialize_enum_u8(r, pdu.multi_slot_tx_indicator);
  pdu.pi2_bpsk = r.read_bool();
  deserialize(r, pdu.prbs);
  deserialize(r, pdu.symbols);
  pdu.intra_slot_frequency_hopping = r.read_bool();
  pdu.second_hop_prb               = r.read_u16();
  deserialize_enum_u8(r, pdu.pucch_grp_hopping);
  pdu.reserved              = r.read_u8();
  pdu.nid_pucch_hopping     = r.read_u16();
  pdu.initial_cyclic_shift  = r.read_u16();
  pdu.nid_pucch_scrambling  = r.read_u16();
  pdu.time_domain_occ_index = r.read_u8();
  pdu.pre_dft_occ_idx       = r.read_u8();
  pdu.pre_dft_occ_len       = r.read_u8();
  pdu.add_dmrs_flag         = r.read_bool();
  pdu.nid0_pucch_dmrs_scrambling = r.read_u16();
  pdu.m0_pucch_dmrs_cyclic_shift = r.read_u8();
  pdu.sr_bit_len            = r.read_u8();
  pdu.bit_len_harq          = r.read_u16();
  pdu.csi_part1_bit_length  = r.read_u16();
  pdu.pucch_maintenance_v3.max_code_rate = r.read_u8();
  pdu.pucch_maintenance_v3.ul_bwp_id     = r.read_u8();
  deserialize_uci_correspondence(r, pdu.uci_correspondence);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_srs_pdu& pdu)
{
  serialize(w, pdu.rnti);
  w.write_u32(pdu.handle);
  serialize(w, pdu.bwp);
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  serialize_enum_u8(w, pdu.num_ant_ports);
  serialize(w, pdu.ofdm_symbols);
  serialize_enum_u8(w, pdu.num_repetitions);
  w.write_u8(pdu.time_start_position);
  w.write_u8(pdu.config_index);
  w.write_u16(pdu.sequence_id);
  w.write_u8(pdu.bandwidth_index);
  serialize_enum_u8(w, pdu.comb_size);
  w.write_u8(pdu.comb_offset);
  w.write_u8(pdu.cyclic_shift);
  w.write_u8(pdu.frequency_position);
  w.write_u16(pdu.frequency_shift);
  w.write_u8(pdu.frequency_hopping);
  serialize_enum_u8(w, pdu.group_or_sequence_hopping);
  serialize_enum_u8(w, pdu.resource_type);
  serialize_enum_u16(w, pdu.t_srs);
  w.write_u16(pdu.t_offset);
  w.write_bool(pdu.enable_normalized_iq_matrix_report);
  w.write_bool(pdu.enable_positioning_report);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_srs_pdu& pdu)
{
  deserialize(r, pdu.rnti);
  pdu.handle = r.read_u32();
  deserialize(r, pdu.bwp);
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  deserialize_enum_u8(r, pdu.num_ant_ports);
  deserialize(r, pdu.ofdm_symbols);
  deserialize_enum_u8(r, pdu.num_repetitions);
  pdu.time_start_position = r.read_u8();
  pdu.config_index        = r.read_u8();
  pdu.sequence_id         = r.read_u16();
  pdu.bandwidth_index     = r.read_u8();
  deserialize_enum_u8(r, pdu.comb_size);
  pdu.comb_offset    = r.read_u8();
  pdu.cyclic_shift   = r.read_u8();
  pdu.frequency_position = r.read_u8();
  pdu.frequency_shift    = r.read_u16();
  pdu.frequency_hopping  = r.read_u8();
  deserialize_enum_u8(r, pdu.group_or_sequence_hopping);
  deserialize_enum_u8(r, pdu.resource_type);
  deserialize_enum_u16(r, pdu.t_srs);
  pdu.t_offset = r.read_u16();
  pdu.enable_normalized_iq_matrix_report = r.read_bool();
  pdu.enable_positioning_report          = r.read_bool();
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_tti_request_pdu& pdu)
{
  serialize_enum_u16(w, pdu.pdu_type);
  w.write_u16(pdu.pdu_size);
  serialize(w, pdu.prach_pdu);
  serialize(w, pdu.pusch_pdu);
  serialize(w, pdu.pucch_pdu);
  serialize(w, pdu.srs_pdu);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_tti_request_pdu& pdu)
{
  deserialize_enum_u16(r, pdu.pdu_type);
  pdu.pdu_size = r.read_u16();
  deserialize(r, pdu.prach_pdu);
  deserialize(r, pdu.pusch_pdu);
  deserialize(r, pdu.pucch_pdu);
  deserialize(r, pdu.srs_pdu);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_tti_request& msg)
{
  serialize(w, msg.slot);
  for (unsigned i = 0; i < fapi::ul_tti_request::MAX_NUM_UL_TYPES; ++i) {
    w.write_u16(msg.num_pdus_of_each_type[i]);
  }
  w.write_u16(msg.num_groups);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_tti_request& msg)
{
  deserialize(r, msg.slot);
  for (unsigned i = 0; i < fapi::ul_tti_request::MAX_NUM_UL_TYPES; ++i) {
    msg.num_pdus_of_each_type[i] = r.read_u16();
  }
  msg.num_groups = r.read_u16();
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::ul_tti_request_pdu pdu{};
    deserialize(r, pdu);
    msg.pdus.push_back(std::move(pdu));
  }
}
