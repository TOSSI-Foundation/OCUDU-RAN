#include "fapi_serializer_p7_dl.h"
#include "ocudu/ran/pdcch/pdcch_context.h"
#include "ocudu/ran/pdcch/search_space.h"
#include "ocudu/ran/pdsch/pdsch_context.h"

using namespace ocudu;
using namespace ocudu::fapi_serial;


void fapi_serial::serialize(buffer_writer& w, const pdcch_context& ctx)
{
  w.write_u8(static_cast<uint8_t>(ctx.get_ss_id()));
  const char* fmt = ctx.get_dci_format();
  if (fmt != nullptr) {
    uint16_t len = static_cast<uint16_t>(std::strlen(fmt));
    w.write_u16(len);
    w.write_bytes(fmt, len);
  } else {
    w.write_u16(0);
  }
  const auto& hft = ctx.get_harq_feedback_timing();
  if (hft.has_value()) {
    w.write_u8(1);
    w.write_u32(hft.value());
  } else {
    w.write_u8(0);
  }
}

void fapi_serial::deserialize(buffer_reader& r, pdcch_context& ctx)
{
  search_space_id ss_id = static_cast<search_space_id>(r.read_u8());
  uint16_t        fmt_len = r.read_u16();

  if (fmt_len > 0) {
    r.skip(fmt_len);
  }
  std::optional<unsigned> harq_feedback_timing;
  uint8_t                 has_hft = r.read_u8();
  if (has_hft) {
    harq_feedback_timing = r.read_u32();
  }
  ctx = pdcch_context(ss_id, nullptr, harq_feedback_timing);
}


void fapi_serial::serialize(buffer_writer& w, const pdsch_context& ctx)
{
  serialize(w, ctx.get_h_id());
  w.write_u32(ctx.get_k1());
  w.write_u32(ctx.get_nof_retxs());
}

void fapi_serial::deserialize(buffer_reader& r, pdsch_context& ctx)
{
  harq_id_t h_id;
  deserialize(r, h_id);
  unsigned k1        = r.read_u32();
  unsigned nof_retxs = r.read_u32();
  ctx = pdsch_context(h_id, k1, nof_retxs);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::tx_precoding_and_beamforming_pdu& pdu)
{
  w.write_u16(pdu.prg_size);
  w.write_u16(pdu.prg.pm_index);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::tx_precoding_and_beamforming_pdu& pdu)
{
  pdu.prg_size      = r.read_u16();
  pdu.prg.pm_index  = r.read_u16();
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_dci_pdu& pdu)
{
  serialize(w, pdu.rnti);
  w.write_u16(pdu.nid_pdcch_data);
  w.write_u16(pdu.nid_pdcch_dmrs);
  w.write_u16(pdu.nrnti_pdcch_data);
  w.write_u8(pdu.cce_index);
  serialize_enum_u8(w, pdu.dci_aggregation_level);
  serialize(w, pdu.precoding_and_beamforming);

  uint8_t power_idx = static_cast<uint8_t>(pdu.power_config.index());
  w.write_u8(power_idx);
  if (power_idx == 0) {
    const auto& nr = std::get<fapi::dl_dci_pdu::power_profile_nr>(pdu.power_config);
    w.write_i8(nr.power_control_offset_ss);
  } else {
    const auto& sss = std::get<fapi::dl_dci_pdu::power_profile_sss>(pdu.power_config);
    w.write_float(sss.dmrs_power_offset_db);
    w.write_float(sss.data_power_offset_db);
  }

  serialize(w, pdu.payload);

  if (pdu.context.has_value()) {
    w.write_u8(1);
    serialize(w, pdu.context.value());
  } else {
    w.write_u8(0);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_dci_pdu& pdu)
{
  deserialize(r, pdu.rnti);
  pdu.nid_pdcch_data  = r.read_u16();
  pdu.nid_pdcch_dmrs  = r.read_u16();
  pdu.nrnti_pdcch_data = r.read_u16();
  pdu.cce_index       = r.read_u8();
  deserialize_enum_u8(r, pdu.dci_aggregation_level);
  deserialize(r, pdu.precoding_and_beamforming);

  uint8_t power_idx = r.read_u8();
  if (power_idx == 0) {
    fapi::dl_dci_pdu::power_profile_nr nr;
    nr.power_control_offset_ss = r.read_i8();
    pdu.power_config = nr;
  } else {
    fapi::dl_dci_pdu::power_profile_sss sss;
    sss.dmrs_power_offset_db = r.read_float();
    sss.data_power_offset_db = r.read_float();
    pdu.power_config = sss;
  }

  deserialize(r, pdu.payload);

  uint8_t has_ctx = r.read_u8();
  if (has_ctx) {
    pdcch_context ctx;
    deserialize(r, ctx);
    pdu.context = std::move(ctx);
  } else {
    pdu.context = std::nullopt;
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_pdcch_pdu& pdu)
{
  serialize(w, pdu.coreset_bwp);
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  serialize(w, pdu.symbols);
  serialize(w, pdu.freq_domain_resource);

  uint8_t mapping_idx = static_cast<uint8_t>(pdu.mapping.index());
  w.write_u8(mapping_idx);
  if (mapping_idx == 0) {
    const auto& m = std::get<fapi::dl_pdcch_pdu::mapping_coreset_0>(pdu.mapping);
    w.write_u8(m.interleaved.reg_bundle_sz);
    w.write_u8(m.interleaved.interleaver_sz);
    w.write_u16(m.interleaved.shift_index);
  } else if (mapping_idx == 1) {
    const auto& m = std::get<fapi::dl_pdcch_pdu::mapping_interleaved>(pdu.mapping);
    w.write_u8(m.interleaved.reg_bundle_sz);
    w.write_u8(m.interleaved.interleaver_sz);
    w.write_u16(m.interleaved.shift_index);
  } else {
    const auto& m = std::get<fapi::dl_pdcch_pdu::mapping_non_interleaved>(pdu.mapping);
    w.write_u8(m.reg_bundle_sz);
  }

  serialize_enum_u8(w, pdu.precoder_granularity);
  serialize(w, pdu.dl_dci);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_pdcch_pdu& pdu)
{
  deserialize(r, pdu.coreset_bwp);
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  deserialize(r, pdu.symbols);
  deserialize(r, pdu.freq_domain_resource);

  uint8_t mapping_idx = r.read_u8();
  if (mapping_idx == 0) {
    fapi::dl_pdcch_pdu::mapping_coreset_0 m;
    m.interleaved.reg_bundle_sz  = r.read_u8();
    m.interleaved.interleaver_sz = r.read_u8();
    m.interleaved.shift_index    = r.read_u16();
    pdu.mapping = m;
  } else if (mapping_idx == 1) {
    fapi::dl_pdcch_pdu::mapping_interleaved m;
    m.interleaved.reg_bundle_sz  = r.read_u8();
    m.interleaved.interleaver_sz = r.read_u8();
    m.interleaved.shift_index    = r.read_u16();
    pdu.mapping = m;
  } else {
    fapi::dl_pdcch_pdu::mapping_non_interleaved m;
    m.reg_bundle_sz = r.read_u8();
    pdu.mapping = m;
  }

  deserialize_enum_u8(r, pdu.precoder_granularity);
  deserialize(r, pdu.dl_dci);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_pdsch_codeword& cw)
{
  w.write_u16(cw.target_code_rate);
  w.write_u8(cw.qam_mod_order);
  w.write_u8(cw.mcs_index);
  w.write_u8(cw.mcs_table);
  w.write_u8(cw.rv_index);
  serialize(w, cw.tb_size);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_pdsch_codeword& cw)
{
  cw.target_code_rate = r.read_u16();
  cw.qam_mod_order    = r.read_u8();
  cw.mcs_index        = r.read_u8();
  cw.mcs_table        = r.read_u8();
  cw.rv_index         = r.read_u8();
  deserialize(r, cw.tb_size);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_pdsch_maintenance_parameters_v3& p)
{
  serialize_enum_u8(w, p.trans_type);
  w.write_u16(p.coreset_start_point);
  w.write_u16(p.initial_dl_bwp_size);
  serialize_enum_u8(w, p.ldpc_base_graph);
  serialize(w, p.tb_size_lbrm_bytes);
  w.write_u8(p.tb_crc_required);
  for (unsigned i = 0; i < fapi::dl_pdsch_maintenance_parameters_v3::MAX_SIZE_SSB_PDU_FOR_RM; ++i) {
    w.write_u16(p.ssb_pdus_for_rate_matching[i]);
  }
  w.write_u16(p.ssb_config_for_rate_matching);
  w.write_u8(p.prb_sym_rm_pattern_bitmap_size_byref);
  serialize_static_vector_u8(w, p.prb_sym_rm_patt_bmp_byref);
  w.write_u8(p.num_prb_sym_rm_patts_by_value);
  w.write_u8(p.num_coreset_rm_patterns);
  w.write_u16(p.pdcch_pdu_index);
  w.write_u16(p.dci_index);
  w.write_u8(p.lte_crs_rm_pattern_bitmap_size);
  serialize_static_vector_u8(w, p.lte_crs_rm_pattern);
  serialize_static_vector_u16(w, p.csi_for_rm);
  w.write_u8(p.max_num_cbg_per_tb);
  serialize_static_vector_u8(w, p.cbg_tx_information);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_pdsch_maintenance_parameters_v3& p)
{
  deserialize_enum_u8(r, p.trans_type);
  p.coreset_start_point = r.read_u16();
  p.initial_dl_bwp_size = r.read_u16();
  deserialize_enum_u8(r, p.ldpc_base_graph);
  deserialize(r, p.tb_size_lbrm_bytes);
  p.tb_crc_required = r.read_u8();
  for (unsigned i = 0; i < fapi::dl_pdsch_maintenance_parameters_v3::MAX_SIZE_SSB_PDU_FOR_RM; ++i) {
    p.ssb_pdus_for_rate_matching[i] = r.read_u16();
  }
  p.ssb_config_for_rate_matching = r.read_u16();
  p.prb_sym_rm_pattern_bitmap_size_byref = r.read_u8();
  deserialize_static_vector_u8(r, p.prb_sym_rm_patt_bmp_byref);
  p.num_prb_sym_rm_patts_by_value = r.read_u8();
  p.num_coreset_rm_patterns       = r.read_u8();
  p.pdcch_pdu_index = r.read_u16();
  p.dci_index       = r.read_u16();
  p.lte_crs_rm_pattern_bitmap_size = r.read_u8();
  deserialize_static_vector_u8(r, p.lte_crs_rm_pattern);
  deserialize_static_vector_u16(r, p.csi_for_rm);
  p.max_num_cbg_per_tb = r.read_u8();
  deserialize_static_vector_u8(r, p.cbg_tx_information);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_pdsch_parameters_v4& p)
{
  w.write_u8(p.coreset_rm_pattern_bitmap_size_by_ref);
  serialize_static_vector_u8(w, p.coreset_rm_pattern_bmp_by_ref);
  w.write_u8(p.lte_crs_mbsfn_derivation_method);
  serialize_static_vector_u8(w, p.lte_crs_mbsfn_pattern);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_pdsch_parameters_v4& p)
{
  p.coreset_rm_pattern_bitmap_size_by_ref = r.read_u8();
  deserialize_static_vector_u8(r, p.coreset_rm_pattern_bmp_by_ref);
  p.lte_crs_mbsfn_derivation_method = r.read_u8();
  deserialize_static_vector_u8(r, p.lte_crs_mbsfn_pattern);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_pdsch_pdu& pdu)
{
  serialize(w, pdu.pdu_bitmap);
  serialize(w, pdu.rnti);
  w.write_u16(pdu.pdu_index);
  serialize(w, pdu.bwp);
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  uint16_t nof_cws = static_cast<uint16_t>(pdu.cws.size());
  w.write_u16(nof_cws);
  for (uint16_t i = 0; i < nof_cws; ++i) {
    serialize(w, pdu.cws[i]);
  }
  w.write_u16(pdu.nid_pdsch);
  w.write_u8(pdu.num_layers);
  w.write_u8(pdu.transmission_scheme);
  serialize_enum_u8(w, pdu.ref_point);
  w.write_u16(pdu.dl_dmrs_symb_pos);
  w.write_u16(pdu.pdsch_dmrs_scrambling_id);
  serialize_enum_u8(w, pdu.dmrs_type);
  w.write_u16(pdu.pdsch_dmrs_scrambling_id_compl);
  serialize_enum_u8(w, pdu.low_papr_dmrs);
  w.write_u8(pdu.nscid);
  w.write_u8(pdu.num_dmrs_cdm_grps_no_data);
  w.write_u16(pdu.dmrs_ports);
  serialize_enum_u8(w, pdu.resource_alloc);
  w.write_bytes(pdu.rb_bitmap.data(), fapi::dl_pdsch_pdu::MAX_SIZE_RB_BITMAP);
  serialize(w, pdu.vrbs);
  serialize_enum_u8(w, pdu.vrb_to_prb_mapping);
  serialize(w, pdu.symbols);

  uint8_t power_idx = static_cast<uint8_t>(pdu.power_config.index());
  w.write_u8(power_idx);
  if (power_idx == 0) {
    const auto& nr = std::get<fapi::dl_pdsch_pdu::power_profile_nr>(pdu.power_config);
    w.write_i32(nr.power_control_offset_profile_nr);
    serialize_enum_u8(w, nr.power_control_offset_ss_profile_nr);
  } else {
    const auto& sss = std::get<fapi::dl_pdsch_pdu::power_profile_sss>(pdu.power_config);
    w.write_float(sss.dmrs_power_offset_sss_db);
    w.write_float(sss.data_power_offset_sss_db);
  }

  serialize(w, pdu.precoding_and_beamforming);
  w.write_u8(pdu.is_last_cb_present);
  serialize_enum_u8(w, pdu.is_inline_tb_crc);
  for (unsigned i = 0; i < fapi::dl_pdsch_pdu::MAX_SIZE_DL_TB_CRC; ++i) {
    w.write_u32(pdu.dl_tb_crc_cw[i]);
  }
  serialize(w, pdu.pdsch_maintenance_v3);
  serialize(w, pdu.pdsch_parameters_v4);

  if (pdu.context.has_value()) {
    w.write_u8(1);
    serialize(w, pdu.context.value());
  } else {
    w.write_u8(0);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_pdsch_pdu& pdu)
{
  deserialize(r, pdu.pdu_bitmap);
  deserialize(r, pdu.rnti);
  pdu.pdu_index = r.read_u16();
  deserialize(r, pdu.bwp);
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  uint16_t nof_cws = r.read_u16();
  pdu.cws.clear();
  for (uint16_t i = 0; i < nof_cws; ++i) {
    fapi::dl_pdsch_codeword cw{};
    deserialize(r, cw);
    pdu.cws.push_back(std::move(cw));
  }
  pdu.nid_pdsch = r.read_u16();
  pdu.num_layers = r.read_u8();
  pdu.transmission_scheme = r.read_u8();
  deserialize_enum_u8(r, pdu.ref_point);
  pdu.dl_dmrs_symb_pos = r.read_u16();
  pdu.pdsch_dmrs_scrambling_id = r.read_u16();
  deserialize_enum_u8(r, pdu.dmrs_type);
  pdu.pdsch_dmrs_scrambling_id_compl = r.read_u16();
  deserialize_enum_u8(r, pdu.low_papr_dmrs);
  pdu.nscid = r.read_u8();
  pdu.num_dmrs_cdm_grps_no_data = r.read_u8();
  pdu.dmrs_ports = r.read_u16();
  deserialize_enum_u8(r, pdu.resource_alloc);
  r.read_bytes(pdu.rb_bitmap.data(), fapi::dl_pdsch_pdu::MAX_SIZE_RB_BITMAP);
  deserialize(r, pdu.vrbs);
  deserialize_enum_u8(r, pdu.vrb_to_prb_mapping);
  deserialize(r, pdu.symbols);

  uint8_t power_idx = r.read_u8();
  if (power_idx == 0) {
    fapi::dl_pdsch_pdu::power_profile_nr nr;
    nr.power_control_offset_profile_nr    = r.read_i32();
    deserialize_enum_u8(r, nr.power_control_offset_ss_profile_nr);
    pdu.power_config = nr;
  } else {
    fapi::dl_pdsch_pdu::power_profile_sss sss;
    sss.dmrs_power_offset_sss_db = r.read_float();
    sss.data_power_offset_sss_db = r.read_float();
    pdu.power_config = sss;
  }

  deserialize(r, pdu.precoding_and_beamforming);
  pdu.is_last_cb_present = r.read_u8();
  deserialize_enum_u8(r, pdu.is_inline_tb_crc);
  for (unsigned i = 0; i < fapi::dl_pdsch_pdu::MAX_SIZE_DL_TB_CRC; ++i) {
    pdu.dl_tb_crc_cw[i] = r.read_u32();
  }
  deserialize(r, pdu.pdsch_maintenance_v3);
  deserialize(r, pdu.pdsch_parameters_v4);

  uint8_t has_ctx = r.read_u8();
  if (has_ctx) {
    pdsch_context ctx;
    deserialize(r, ctx);
    pdu.context = std::move(ctx);
  } else {
    pdu.context = std::nullopt;
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_csi_rs_pdu& pdu)
{
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  serialize(w, pdu.crbs);
  serialize_enum_u8(w, pdu.type);
  w.write_u8(pdu.row);
  serialize(w, pdu.freq_domain);
  w.write_u8(pdu.symb_L0);
  w.write_u8(pdu.symb_L1);
  serialize_enum_u8(w, pdu.cdm_type);
  serialize_enum_u8(w, pdu.freq_density);
  w.write_u16(pdu.scramb_id);
  w.write_i32(pdu.power_control_offset_profile_nr);
  serialize_enum_u8(w, pdu.power_control_offset_ss_profile_nr);
  serialize(w, pdu.bwp);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_csi_rs_pdu& pdu)
{
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  deserialize(r, pdu.crbs);
  deserialize_enum_u8(r, pdu.type);
  pdu.row = r.read_u8();
  deserialize(r, pdu.freq_domain);
  pdu.symb_L0 = r.read_u8();
  pdu.symb_L1 = r.read_u8();
  deserialize_enum_u8(r, pdu.cdm_type);
  deserialize_enum_u8(r, pdu.freq_density);
  pdu.scramb_id = r.read_u16();
  pdu.power_control_offset_profile_nr = r.read_i32();
  deserialize_enum_u8(r, pdu.power_control_offset_ss_profile_nr);
  deserialize(r, pdu.bwp);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_ssb_pdu& pdu)
{
  w.write_u16(pdu.phys_cell_id);
  serialize_enum_u8(w, pdu.beta_pss_profile_nr);
  w.write_u8(static_cast<uint8_t>(pdu.ssb_block_index));
  w.write_u16(pdu.subcarrier_offset.value());
  w.write_u16(pdu.ssb_offset_pointA.value());
  w.write_u32(pdu.bch_payload);
  serialize_enum_u8(w, pdu.case_type);
  serialize(w, pdu.scs);
  w.write_u8(pdu.L_max);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_ssb_pdu& pdu)
{
  pdu.phys_cell_id = r.read_u16();
  deserialize_enum_u8(r, pdu.beta_pss_profile_nr);
  pdu.ssb_block_index   = static_cast<ssb_id_t>(r.read_u8());
  pdu.subcarrier_offset = ssb_subcarrier_offset(r.read_u16());
  pdu.ssb_offset_pointA = ssb_offset_to_pointA(r.read_u16());
  pdu.bch_payload       = r.read_u32();
  deserialize_enum_u8(r, pdu.case_type);
  deserialize(r, pdu.scs);
  pdu.L_max = r.read_u8();
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_prs_pdu& pdu)
{
  serialize(w, pdu.scs);
  serialize(w, pdu.cp);
  w.write_u16(pdu.nid_prs);
  serialize_enum_u8(w, pdu.comb_size);
  w.write_u8(pdu.comb_offset);
  serialize_enum_u8(w, pdu.num_symbols);
  w.write_u8(pdu.first_symbol);
  serialize(w, pdu.crbs);
  if (pdu.prs_power_offset.has_value()) {
    w.write_u8(1);
    w.write_float(pdu.prs_power_offset.value());
  } else {
    w.write_u8(0);
  }
  serialize(w, pdu.precoding_and_beamforming);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_prs_pdu& pdu)
{
  deserialize(r, pdu.scs);
  deserialize(r, pdu.cp);
  pdu.nid_prs = r.read_u16();
  deserialize_enum_u8(r, pdu.comb_size);
  pdu.comb_offset = r.read_u8();
  deserialize_enum_u8(r, pdu.num_symbols);
  pdu.first_symbol = r.read_u8();
  deserialize(r, pdu.crbs);
  uint8_t has_power = r.read_u8();
  if (has_power) {
    pdu.prs_power_offset = r.read_float();
  } else {
    pdu.prs_power_offset = std::nullopt;
  }
  deserialize(r, pdu.precoding_and_beamforming);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_tti_request_pdu& pdu)
{
  serialize_enum_u16(w, pdu.pdu_type);

  serialize(w, pdu.pdcch_pdu);
  serialize(w, pdu.pdsch_pdu);
  serialize(w, pdu.csi_rs_pdu);
  serialize(w, pdu.ssb_pdu);
  serialize(w, pdu.prs_pdu);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_tti_request_pdu& pdu)
{
  deserialize_enum_u16(r, pdu.pdu_type);
  deserialize(r, pdu.pdcch_pdu);
  deserialize(r, pdu.pdsch_pdu);
  deserialize(r, pdu.csi_rs_pdu);
  deserialize(r, pdu.ssb_pdu);
  deserialize(r, pdu.prs_pdu);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::dl_tti_request& msg)
{
  serialize(w, msg.slot);
  for (unsigned i = 0; i < fapi::dl_tti_request::MAX_NUM_DL_TYPES; ++i) {
    w.write_u16(msg.num_pdus_of_each_type[i]);
  }
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::dl_tti_request& msg)
{
  deserialize(r, msg.slot);
  for (unsigned i = 0; i < fapi::dl_tti_request::MAX_NUM_DL_TYPES; ++i) {
    msg.num_pdus_of_each_type[i] = r.read_u16();
  }
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::dl_tti_request_pdu pdu{};
    deserialize(r, pdu);
    msg.pdus.push_back(std::move(pdu));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::tx_data_req_pdu& pdu)
{
  w.write_u16(pdu.pdu_index);
  w.write_u8(pdu.cw_index);
  serialize_shared_transport_block(w, pdu.pdu);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::tx_data_req_pdu& pdu, std::vector<uint8_t>& tb_storage)
{
  pdu.pdu_index = r.read_u16();
  pdu.cw_index  = r.read_u8();
  uint32_t size = r.read_u32();
  tb_storage.resize(size);
  if (size > 0) {
    r.read_bytes(tb_storage.data(), size);
  }
  pdu.pdu = shared_transport_block(span<const uint8_t>(tb_storage.data(), tb_storage.size()));
}


void fapi_serial::serialize(buffer_writer& w, const fapi::tx_data_request& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::tx_data_request& msg,
                              std::vector<std::vector<uint8_t>>& tb_storages)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  tb_storages.resize(count);
  for (uint16_t i = 0; i < count; ++i) {
    fapi::tx_data_req_pdu pdu{};
    deserialize(r, pdu, tb_storages[i]);
    msg.pdus.push_back(std::move(pdu));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_dci_pdu& pdu)
{
  serialize(w, pdu.pdu);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_dci_pdu& pdu)
{
  deserialize(r, pdu.pdu);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::ul_dci_request& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::ul_dci_request& msg)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::ul_dci_pdu pdu{};
    deserialize(r, pdu);
    msg.pdus.push_back(std::move(pdu));
  }
}
