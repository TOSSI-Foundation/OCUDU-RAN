#include "fapi_stats_serializers.h"

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <variant>

namespace ocudu::fapi_stats {

namespace {


template <typename... Args>
bool appendf(char* out, int max_len, int& off, const char* fmt, Args... args)
{
  if (off >= max_len - 1) {
    return false;
  }
  int remaining = max_len - off;
  int n;
  if constexpr (sizeof...(Args) == 0) {
    n = std::snprintf(out + off, remaining, "%s", fmt);
  } else {
    n = std::snprintf(out + off, remaining, fmt, args...);
  }
  if (n < 0) {
    return false;
  }
  if (n >= remaining) {
    off = max_len - 1;
    return false;
  }
  off += n;
  return true;
}

template <typename E>
std::enable_if_t<std::is_enum_v<E>, int> to_int(E e)
{
  return static_cast<int>(static_cast<typename std::underlying_type<E>::type>(e));
}


void append_hex(char* out, int max_len, int& off, const uint8_t* data, size_t size, size_t max_bytes = 128)
{
  const size_t n = size < max_bytes ? size : max_bytes;
  for (size_t i = 0; i < n; ++i) {
    if (!appendf(out, max_len, off, "%02x", data[i])) {
      return;
    }
  }
  if (size > max_bytes) {
    appendf(out, max_len, off, "...(truncated %zu of %zu)", n, size);
  }
}

} // namespace



int serialize_param_request(char* out, int max_len, const fapi::param_request& /*msg*/)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=PARAM_REQUEST\n");
  return 0;
}

int serialize_param_response(char* out, int max_len, const fapi::param_response& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=PARAM_RESPONSE\nerror_code=%d\n", to_int(msg.error_code));
  return 0;
}

int serialize_config_request(char* out, int max_len, const fapi::config_request& msg)
{
  int         off = 0;
  const auto& cc  = msg.cell_cfg;
  appendf(out, max_len, off, "msg_type=CONFIG_REQUEST\n");
  appendf(out, max_len, off, "cell_cfg.pci=%u\n", static_cast<unsigned>(cc.pci));
  appendf(out, max_len, off, "cell_cfg.scs_common=%d\n", to_int(cc.scs_common));
  appendf(out, max_len, off, "cell_cfg.cp=%d\n", to_int(cc.cp.value));
  appendf(out, max_len, off, "cell_cfg.duplex=%d\n", to_int(cc.duplex));
  appendf(out, max_len, off, "cell_cfg.has_tdd_cfg=%d\n", static_cast<int>(cc.tdd_ul_dl_cfg_common.has_value()));
  appendf(out, max_len, off, "carrier_cfg.dl_bandwidth=%u\n", cc.carrier_cfg.dl_bandwidth);
  appendf(out, max_len, off, "carrier_cfg.dl_f_ref_arfcn=%u\n", cc.carrier_cfg.dl_f_ref_arfcn);
  appendf(out, max_len, off, "carrier_cfg.ul_bandwidth=%u\n", cc.carrier_cfg.ul_bandwidth);
  appendf(out, max_len, off, "carrier_cfg.ul_f_ref_arfcn=%u\n", cc.carrier_cfg.ul_f_ref_arfcn);
  appendf(out, max_len, off, "carrier_cfg.num_tx_ant=%u\n", cc.carrier_cfg.num_tx_ant);
  appendf(out, max_len, off, "carrier_cfg.num_rx_ant=%u\n", cc.carrier_cfg.num_rx_ant);
  return 0;
}

int serialize_config_response(char* out, int max_len, const fapi::config_response& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=CONFIG_RESPONSE\nerror_code=%d\n", to_int(msg.error_code));
  return 0;
}

int serialize_start_request(char* out, int max_len, const fapi::start_request& /*msg*/)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=START_REQUEST\n");
  return 0;
}

int serialize_stop_request(char* out, int max_len, const fapi::stop_request& /*msg*/)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=STOP_REQUEST\n");
  return 0;
}

int serialize_stop_indication(char* out, int max_len, const fapi::stop_indication& /*msg*/)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=STOP_INDICATION\n");
  return 0;
}

int serialize_error_indication(char* out, int max_len, const fapi::error_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=ERROR_INDICATION\n");
  appendf(out, max_len, off, "message_id=0x%02x\n", to_int(msg.message_id));
  appendf(out, max_len, off, "error_code=%d\n", to_int(msg.error_code));
  if (msg.slot.has_value()) {
    appendf(out, max_len, off, "SFN=%u\nSlot=%u\n", msg.slot->sfn(), msg.slot->slot_index());
  }
  if (msg.expected_slot.has_value()) {
    appendf(out, max_len, off, "expected_SFN=%u\nexpected_Slot=%u\n",
            msg.expected_slot->sfn(), msg.expected_slot->slot_index());
  }
  return 0;
}


namespace {

struct pdcch_mapping_summary {
  uint8_t coreset_type = 0;
  uint8_t cce_reg_mapping_type = 0;
  uint8_t reg_bundle_size = 0;
  uint8_t interleaver_size = 0;
  uint8_t shift_index = 0;
};

pdcch_mapping_summary summarize_pdcch_mapping(const fapi::dl_pdcch_pdu& pdu)
{
  pdcch_mapping_summary s;
  std::visit(
      [&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, fapi::dl_pdcch_pdu::mapping_coreset_0>) {
          s.coreset_type         = 0;
          s.cce_reg_mapping_type = 1; // CORESET0 always interleaved
          s.reg_bundle_size      = m.interleaved.reg_bundle_sz;
          s.interleaver_size     = m.interleaved.interleaver_sz;
          s.shift_index          = static_cast<uint8_t>(m.interleaved.shift_index);
        } else if constexpr (std::is_same_v<T, fapi::dl_pdcch_pdu::mapping_interleaved>) {
          s.coreset_type         = 1;
          s.cce_reg_mapping_type = 1;
          s.reg_bundle_size      = m.interleaved.reg_bundle_sz;
          s.interleaver_size     = m.interleaved.interleaver_sz;
          s.shift_index          = static_cast<uint8_t>(m.interleaved.shift_index);
        } else if constexpr (std::is_same_v<T, fapi::dl_pdcch_pdu::mapping_non_interleaved>) {
          s.coreset_type         = 1;
          s.cce_reg_mapping_type = 0;
          s.reg_bundle_size      = m.reg_bundle_sz;
        }
      },
      pdu.mapping);
  return s;
}

void append_pdcch_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::dl_pdcch_pdu& pdu)
{
  const auto map = summarize_pdcch_mapping(pdu);

  appendf(out, max_len, off, "\n----pdcch pdu----\n");
  appendf(out, max_len, off, "BWPSize=%u\n", pdu.coreset_bwp.length());
  appendf(out, max_len, off, "BWPStart=%u\n", pdu.coreset_bwp.start());
  appendf(out, max_len, off, "SubcarrierSpacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "CyclicPrefix=%d\n", to_int(pdu.cp.value));
  appendf(out, max_len, off, "StartSymbolIndex=%u\n", pdu.symbols.start());
  appendf(out, max_len, off, "DurationSymbols=%u\n", pdu.symbols.length());
  appendf(out, max_len, off, "CceRegMappingType=%u\n", map.cce_reg_mapping_type);
  appendf(out, max_len, off, "RegBundleSize=%u\n", map.reg_bundle_size);
  appendf(out, max_len, off, "InterleaverSize=%u\n", map.interleaver_size);
  appendf(out, max_len, off, "CoreSetType=%u\n", map.coreset_type);
  appendf(out, max_len, off, "ShiftIndex=%u\n", map.shift_index);
  appendf(out, max_len, off, "precoderGranularity=%d\n", to_int(pdu.precoder_granularity));
  appendf(out, max_len, off, "numDlDci=1\n");

  uint8_t power_control_offset_ss = 0;
  std::visit([&](const auto& cfg) {
    using T = std::decay_t<decltype(cfg)>;
    if constexpr (std::is_same_v<T, fapi::dl_dci_pdu::power_profile_nr>) {
      power_control_offset_ss = static_cast<uint8_t>(cfg.power_control_offset_ss);
    }
  }, pdu.dl_dci.power_config);

  appendf(out, max_len, off, "dci[0].RNTI=%u\n", static_cast<unsigned>(pdu.dl_dci.rnti));
  appendf(out, max_len, off, "dci[0].ScramblingId=%u\n", pdu.dl_dci.nid_pdcch_data);
  appendf(out, max_len, off, "dci[0].ScramblingRNTI=%u\n", pdu.dl_dci.nrnti_pdcch_data);
  appendf(out, max_len, off, "dci[0].CceIndex=%u\n", pdu.dl_dci.cce_index);
  appendf(out, max_len, off, "dci[0].AggregationLevel=%d\n", to_int(pdu.dl_dci.dci_aggregation_level));
  appendf(out, max_len, off, "dci[0].beta_PDCCH_1_0=0\n"); // not exposed in our struct
  appendf(out, max_len, off, "dci[0].powerControlOffsetSS=%u\n", power_control_offset_ss);
  appendf(out, max_len, off, "dci[0].PayloadSizeBits=%u\n", static_cast<unsigned>(pdu.dl_dci.payload.size()));
  (void)pdu_idx;
}

void append_pdsch_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::dl_pdsch_pdu& pdu)
{
  appendf(out, max_len, off, "\n----pdsch pdu----\n");
  appendf(out, max_len, off, "pduBitmap=%lu\n", static_cast<unsigned long>(pdu.pdu_bitmap.to_ulong()));
  appendf(out, max_len, off, "rnti=%u\n", static_cast<unsigned>(pdu.rnti));
  appendf(out, max_len, off, "pduIndex=%u\n", pdu.pdu_index);
  appendf(out, max_len, off, "BWPSize=%u\n", pdu.bwp.length());
  appendf(out, max_len, off, "BWPStart=%u\n", pdu.bwp.start());
  appendf(out, max_len, off, "SubcarrierSpacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "CyclicPrefix=%d\n", to_int(pdu.cp.value));
  appendf(out, max_len, off, "NrOfCodewords=%u\n", static_cast<unsigned>(pdu.cws.size()));


  auto cw = [&](size_t i, auto fn) {
    return (i < pdu.cws.size()) ? fn(pdu.cws[i]) : 0u;
  };
  appendf(out, max_len, off, "targetCodeRate[0]=%u, targetCodeRate[1]=%u\n",
          cw(0, [](const auto& c) -> unsigned { return c.target_code_rate; }),
          cw(1, [](const auto& c) -> unsigned { return c.target_code_rate; }));
  appendf(out, max_len, off, "qamModOrder[0]=%u, qamModOrder[1]=%u\n",
          cw(0, [](const auto& c) -> unsigned { return c.qam_mod_order; }),
          cw(1, [](const auto& c) -> unsigned { return c.qam_mod_order; }));
  appendf(out, max_len, off, "mcsIndex[0]=%u, mcsIndex[1]=%u\n",
          cw(0, [](const auto& c) -> unsigned { return c.mcs_index; }),
          cw(1, [](const auto& c) -> unsigned { return c.mcs_index; }));
  appendf(out, max_len, off, "mcsTable[0]=%u, mcsTable[1]=%u\n",
          cw(0, [](const auto& c) -> unsigned { return c.mcs_table; }),
          cw(1, [](const auto& c) -> unsigned { return c.mcs_table; }));
  appendf(out, max_len, off, "rvIndex[0]=%u, rvIndex[1]=%u\n",
          cw(0, [](const auto& c) -> unsigned { return c.rv_index; }),
          cw(1, [](const auto& c) -> unsigned { return c.rv_index; }));
  appendf(out, max_len, off, "TBSize[0]=%u, TBSize[1]=%u\n",
          cw(0, [](const auto& c) -> unsigned { return c.tb_size.value(); }),
          cw(1, [](const auto& c) -> unsigned { return c.tb_size.value(); }));

  appendf(out, max_len, off, "dataScramblingId=%u\n", pdu.nid_pdsch);
  appendf(out, max_len, off, "nrOfLayers=%u\n", pdu.num_layers);
  appendf(out, max_len, off, "transmissionScheme=%u\n", pdu.transmission_scheme);
  appendf(out, max_len, off, "refPoint=%d\n", to_int(pdu.ref_point));
  appendf(out, max_len, off, "dlDmrsSymbPos=%u\n", pdu.dl_dmrs_symb_pos);
  appendf(out, max_len, off, "dmrsConfigType=%d\n", to_int(pdu.dmrs_type));
  appendf(out, max_len, off, "dlDmrsScramblingId=%u\n", pdu.pdsch_dmrs_scrambling_id);
  appendf(out, max_len, off, "SCID=%u\n", pdu.nscid);
  appendf(out, max_len, off, "numDmrsCdmGrpsNoData=%u\n", pdu.num_dmrs_cdm_grps_no_data);
  appendf(out, max_len, off, "dmrsPorts=%u\n", pdu.dmrs_ports);
  appendf(out, max_len, off, "resourceAlloc=%d\n", to_int(pdu.resource_alloc));
  appendf(out, max_len, off, "rbStart=%u\n", pdu.vrbs.start());
  appendf(out, max_len, off, "rbSize=%u\n", pdu.vrbs.length());
  appendf(out, max_len, off, "VRBtoPRBMapping=%d\n", to_int(pdu.vrb_to_prb_mapping));
  appendf(out, max_len, off, "StartSymbolIndex=%u\n", pdu.symbols.start());
  appendf(out, max_len, off, "NrOfSymbols=%u\n", pdu.symbols.length());
  (void)pdu_idx;
}

void append_csi_rs_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::dl_csi_rs_pdu& pdu)
{
  appendf(out, max_len, off, "\n----csi-rs pdu----\n");
  appendf(out, max_len, off, "SubcarrierSpacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "CyclicPrefix=%d\n", to_int(pdu.cp.value));
  appendf(out, max_len, off, "StartRB=%u\n", pdu.crbs.start());
  appendf(out, max_len, off, "NrOfRBs=%u\n", pdu.crbs.length());
  appendf(out, max_len, off, "csiType=%d\n", to_int(pdu.type));
  appendf(out, max_len, off, "row=%u\n", pdu.row);
  appendf(out, max_len, off, "symbL0=%u\n", pdu.symb_L0);
  appendf(out, max_len, off, "symbL1=%u\n", pdu.symb_L1);
  appendf(out, max_len, off, "CDMType=%d\n", to_int(pdu.cdm_type));
  appendf(out, max_len, off, "freqDensity=%d\n", to_int(pdu.freq_density));
  appendf(out, max_len, off, "scrambId=%u\n", pdu.scramb_id);
  (void)pdu_idx;
}

void append_ssb_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::dl_ssb_pdu& pdu)
{
  appendf(out, max_len, off, "\n----ssb pdu----\n");
  appendf(out, max_len, off, "PhysCellId=%u\n", static_cast<unsigned>(pdu.phys_cell_id));
  appendf(out, max_len, off, "BetaPssProfileNr=%d\n", to_int(pdu.beta_pss_profile_nr));
  appendf(out, max_len, off, "ssbBlockIndex=%u\n", static_cast<unsigned>(pdu.ssb_block_index));
  appendf(out, max_len, off, "ssbSubcarrierOffset=%u\n", static_cast<unsigned>(pdu.subcarrier_offset.value()));
  appendf(out, max_len, off, "ssbOffsetPointA=%u\n", static_cast<unsigned>(pdu.ssb_offset_pointA.value()));
  appendf(out, max_len, off, "bchPayload=0x%08x\n", pdu.bch_payload);
  appendf(out, max_len, off, "caseType=%d\n", to_int(pdu.case_type));
  appendf(out, max_len, off, "SubcarrierSpacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "Lmax=%u\n", pdu.L_max);
  (void)pdu_idx;
}

} // namespace

int serialize_dl_tti_request(char* out, int max_len, const fapi::dl_tti_request& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=DL_TTI_REQUEST\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "dl_tti_request_body.nPDUs=%zu\n", msg.pdus.size());
  appendf(out, max_len, off, "dl_tti_request_body.nGroup=0\n");

  const size_t pdu_limit = 3;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    appendf(out, max_len, off, "pdu[%zu].PDUType=%d\npdu[%zu].PDUSize=0\n",
            i, to_int(p.pdu_type), i);
    switch (p.pdu_type) {
      case fapi::dl_pdu_type::PDCCH:  append_pdcch_pdu(out, max_len, off, (int)i, p.pdcch_pdu); break;
      case fapi::dl_pdu_type::PDSCH:  append_pdsch_pdu(out, max_len, off, (int)i, p.pdsch_pdu); break;
      case fapi::dl_pdu_type::CSI_RS: append_csi_rs_pdu(out, max_len, off, (int)i, p.csi_rs_pdu); break;
      case fapi::dl_pdu_type::SSB:    append_ssb_pdu(out, max_len, off, (int)i, p.ssb_pdu); break;
      default: break;
    }
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}


namespace {

void append_prach_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::ul_prach_pdu& pdu)
{
  appendf(out, max_len, off, "\n----prach pdu----\n");
  appendf(out, max_len, off, "num_prach_ocas=%u\n", pdu.num_prach_ocas);
  appendf(out, max_len, off, "prach_format=%d\n", to_int(pdu.prach_format));
  appendf(out, max_len, off, "index_fd_ra=%u\n", pdu.index_fd_ra);
  appendf(out, max_len, off, "prach_start_symbol=%u\n", pdu.prach_start_symbol);
  appendf(out, max_len, off, "num_cs=%u\n", pdu.num_cs);
  appendf(out, max_len, off, "handle=%u\n", pdu.handle);
  appendf(out, max_len, off, "num_fd_ra=%u\n", pdu.num_fd_ra);
  (void)pdu_idx;
}

void append_pusch_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::ul_pusch_pdu& pdu)
{
  appendf(out, max_len, off, "\n----pusch pdu----\n");
  appendf(out, max_len, off, "pdu_bit_map=%lu\n", static_cast<unsigned long>(pdu.pdu_bitmap.to_ulong()));
  appendf(out, max_len, off, "rnti=%u\n", static_cast<unsigned>(pdu.rnti));
  appendf(out, max_len, off, "handle=%u\n", pdu.handle);
  appendf(out, max_len, off, "bwp_size=%u\n", pdu.bwp.length());
  appendf(out, max_len, off, "bwp_start=%u\n", pdu.bwp.start());
  appendf(out, max_len, off, "subcarrier_spacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "cyclic_prefix=%d\n", to_int(pdu.cp.value));
  appendf(out, max_len, off, "target_code_rate=%u\n", pdu.target_code_rate);
  appendf(out, max_len, off, "qam_mod_order=%d\n", to_int(pdu.qam_mod_order));
  appendf(out, max_len, off, "mcs_index=%u\n", pdu.mcs_index);
  appendf(out, max_len, off, "mcs_table=%d\n", to_int(pdu.mcs_table));
  appendf(out, max_len, off, "transform_precoding=%d\n", static_cast<int>(pdu.transform_precoding));
  appendf(out, max_len, off, "data_scrambling_id=%u\n", pdu.nid_pusch);
  appendf(out, max_len, off, "nrOfLayers=%u\n", pdu.num_layers);
  appendf(out, max_len, off, "pusch_data.rv_index=%u\n", pdu.pusch_data.rv_index);
  appendf(out, max_len, off, "pusch_data.harq_process_id=%u\n", pdu.pusch_data.harq_process_id);
  appendf(out, max_len, off, "pusch_data.new_data_indicator=%d\n", static_cast<int>(pdu.pusch_data.new_data));
  appendf(out, max_len, off, "pusch_data.tb_size=%u\n", static_cast<unsigned>(pdu.pusch_data.tb_size.value()));
  appendf(out, max_len, off, "pusch_data.num_cb=%u\n", pdu.pusch_data.num_cb);
  (void)pdu_idx;
}

void append_pucch_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::ul_pucch_pdu& pdu)
{
  appendf(out, max_len, off, "\n----pucch pdu----\n");
  appendf(out, max_len, off, "rnti=%u\n", static_cast<unsigned>(pdu.rnti));
  appendf(out, max_len, off, "handle=%u\n", pdu.handle);
  appendf(out, max_len, off, "bwp_size=%u\n", pdu.bwp.length());
  appendf(out, max_len, off, "bwp_start=%u\n", pdu.bwp.start());
  appendf(out, max_len, off, "subcarrier_spacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "cyclic_prefix=%d\n", to_int(pdu.cp.value));
  appendf(out, max_len, off, "format_type=%d\n", to_int(pdu.format_type));
  appendf(out, max_len, off, "multi_slot_tx_indicator=%d\n", to_int(pdu.multi_slot_tx_indicator));
  appendf(out, max_len, off, "pi_2bpsk=%d\n", static_cast<int>(pdu.pi2_bpsk));
  appendf(out, max_len, off, "prb_start=%u\n", pdu.prbs.start());
  appendf(out, max_len, off, "prb_size=%u\n", pdu.prbs.length());
  appendf(out, max_len, off, "start_symbol_index=%u\n", pdu.symbols.start());
  appendf(out, max_len, off, "nr_of_symbols=%u\n", pdu.symbols.length());
  appendf(out, max_len, off, "freq_hop_flag=%d\n", static_cast<int>(pdu.intra_slot_frequency_hopping));
  appendf(out, max_len, off, "second_hop_prb=%u\n", pdu.second_hop_prb);
  appendf(out, max_len, off, "bit_len_harq=%u\n", pdu.bit_len_harq);
  appendf(out, max_len, off, "bit_len_csi_part1=%u\n", pdu.csi_part1_bit_length);
  appendf(out, max_len, off, "bit_len_csi_part2=0\n"); // not a direct field in our struct
  (void)pdu_idx;
}

void append_srs_pdu(char* out, int max_len, int& off, int pdu_idx, const fapi::ul_srs_pdu& pdu)
{
  appendf(out, max_len, off, "\n----srs pdu----\n");
  appendf(out, max_len, off, "rnti=%u\n", static_cast<unsigned>(pdu.rnti));
  appendf(out, max_len, off, "handle=%u\n", pdu.handle);
  appendf(out, max_len, off, "bwp_size=%u\n", pdu.bwp.length());
  appendf(out, max_len, off, "bwp_start=%u\n", pdu.bwp.start());
  appendf(out, max_len, off, "subcarrier_spacing=%d\n", to_int(pdu.scs));
  appendf(out, max_len, off, "cyclic_prefix=%d\n", to_int(pdu.cp.value));
  appendf(out, max_len, off, "num_ant_ports=%d\n", to_int(pdu.num_ant_ports));
  appendf(out, max_len, off, "start_symbol_index=%u\n", pdu.ofdm_symbols.start());
  appendf(out, max_len, off, "nr_of_symbols=%u\n", pdu.ofdm_symbols.length());
  appendf(out, max_len, off, "config_index=%u\n", pdu.config_index);
  appendf(out, max_len, off, "sequence_id=%u\n", pdu.sequence_id);
  appendf(out, max_len, off, "bandwidth_index=%u\n", pdu.bandwidth_index);
  appendf(out, max_len, off, "comb_size=%d\n", to_int(pdu.comb_size));
  appendf(out, max_len, off, "comb_offset=%u\n", pdu.comb_offset);
  appendf(out, max_len, off, "cyclic_shift=%u\n", pdu.cyclic_shift);
  (void)pdu_idx;
}

} // namespace

int serialize_ul_tti_request(char* out, int max_len, const fapi::ul_tti_request& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=UL_TTI_REQUEST\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());
  const auto& cnt = msg.num_pdus_of_each_type;
  appendf(out, max_len, off, "rach_present=%u\n", cnt.size() > 0 ? cnt[0] : 0);
  appendf(out, max_len, off, "n_ulsch=%u\n", cnt.size() > 1 ? cnt[1] : 0);
  appendf(out, max_len, off, "n_ulcch=%u\n",
          (cnt.size() > 2 ? cnt[2] : 0) + (cnt.size() > 3 ? cnt[3] : 0));
  appendf(out, max_len, off, "n_group=%u\n", msg.num_groups);

  const size_t pdu_limit = 3;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    appendf(out, max_len, off, "pdu[%zu].pdu_type=%d\npdu[%zu].pdu_size=%u\n",
            i, to_int(p.pdu_type), i, p.pdu_size);
    switch (p.pdu_type) {
      case fapi::ul_pdu_type::PRACH: append_prach_pdu(out, max_len, off, (int)i, p.prach_pdu); break;
      case fapi::ul_pdu_type::PUSCH: append_pusch_pdu(out, max_len, off, (int)i, p.pusch_pdu); break;
      case fapi::ul_pdu_type::PUCCH: append_pucch_pdu(out, max_len, off, (int)i, p.pucch_pdu); break;
      case fapi::ul_pdu_type::SRS:   append_srs_pdu(out, max_len, off, (int)i, p.srs_pdu); break;
      default: break;
    }
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_ul_dci_request(char* out, int max_len, const fapi::ul_dci_request& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=UL_DCI_REQUEST\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 3;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    appendf(out, max_len, off, "pdu[%zu].PDUType=0\npdu[%zu].PDUSize=0\n", i, i);
    append_pdcch_pdu(out, max_len, off, (int)i, msg.pdus[i].pdu);
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_tx_data_request(char* out, int max_len, const fapi::tx_data_request& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=TX_DATA_REQUEST\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 3;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p   = msg.pdus[i];
    const auto  buf = p.pdu.get_buffer();
    appendf(out, max_len, off, "TLV[%zu].pdu_index=%u\n", i, p.pdu_index);
    appendf(out, max_len, off, "TLV[%zu].cw_index=%u\n", i, p.cw_index);
    appendf(out, max_len, off, "TLV[%zu].tb_size=%zu\n", i, buf.size());
    appendf(out, max_len, off, "TLV[%zu].data_hex=", i);
    append_hex(out, max_len, off, buf.data(), buf.size());
    appendf(out, max_len, off, "\n");
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}


int serialize_slot_indication(char* out, int max_len, const fapi::slot_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=SLOT_INDICATION\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  return 0;
}

int serialize_rx_data_indication(char* out, int max_len, const fapi::rx_data_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=RX_DATA_INDICATION\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 5;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, p.handle);
    appendf(out, max_len, off, "pdu[%zu].rnti=%u\n", i, static_cast<unsigned>(p.rnti));
    appendf(out, max_len, off, "pdu[%zu].harq_id=%u\n", i, static_cast<unsigned>(p.harq_id));
    appendf(out, max_len, off, "pdu[%zu].pdu_length=%zu\n", i, p.transport_block.size());
    appendf(out, max_len, off, "pdu[%zu].data_hex=", i);
    append_hex(out, max_len, off, p.transport_block.data(), p.transport_block.size());
    appendf(out, max_len, off, "\n");
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_crc_indication(char* out, int max_len, const fapi::crc_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=CRC_INDICATION\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 5;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, p.handle);
    appendf(out, max_len, off, "pdu[%zu].rnti=%u\n", i, static_cast<unsigned>(p.rnti));
    appendf(out, max_len, off, "pdu[%zu].harq_id=%u\n", i, static_cast<unsigned>(p.harq_id));
    appendf(out, max_len, off, "pdu[%zu].tb_crc_status=%d\n", i, static_cast<int>(p.tb_crc_status_ok));
    appendf(out, max_len, off, "pdu[%zu].ul_sinr_metric=%d\n", i, p.ul_sinr_metric);
    appendf(out, max_len, off, "pdu[%zu].rssi=%u\n", i, p.rssi);
    appendf(out, max_len, off, "pdu[%zu].rsrp=%u\n", i, p.rsrp);
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_uci_indication(char* out, int max_len, const fapi::uci_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=UCI_INDICATION\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 5;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    if (std::holds_alternative<fapi::uci_pusch_pdu>(p)) {
      const auto& up = std::get<fapi::uci_pusch_pdu>(p);
      appendf(out, max_len, off, "pdu[%zu].pdu_type=0\n", i); // 0 = PUSCH UCI in XFAPI
      appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, up.handle);
      appendf(out, max_len, off, "pdu[%zu].rnti=%u\n", i, static_cast<unsigned>(up.rnti));
      appendf(out, max_len, off, "pdu[%zu].ul_sinr_metric=%d\n", i, up.ul_sinr_metric);
      appendf(out, max_len, off, "pdu[%zu].rssi=%u\n", i, up.rssi);
      appendf(out, max_len, off, "pdu[%zu].rsrp=%u\n", i, up.rsrp);
      appendf(out, max_len, off, "pdu[%zu].harq[0].harq_value=%d\n", i,
              static_cast<int>(up.harq.has_value()));
      appendf(out, max_len, off, "pdu[%zu].harq_confidence_level=0\n", i);
      appendf(out, max_len, off, "pdu[%zu].sr_indication=0\n", i); // PUSCH-UCI doesn't carry SR
    } else if (std::holds_alternative<fapi::uci_pucch_pdu_format_0_1>(p)) {
      const auto& up = std::get<fapi::uci_pucch_pdu_format_0_1>(p);
      appendf(out, max_len, off, "pdu[%zu].pdu_type=1\n", i); // 1 = PUCCH F0/F1
      appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, up.handle);
      appendf(out, max_len, off, "pdu[%zu].rnti=%u\n", i, static_cast<unsigned>(up.rnti));
      appendf(out, max_len, off, "pdu[%zu].pucch_format=%d\n", i, to_int(up.pucch_format));
      appendf(out, max_len, off, "pdu[%zu].ul_sinr_metric=%d\n", i, up.ul_sinr_metric);
      appendf(out, max_len, off, "pdu[%zu].rssi=%u\n", i, up.rssi);
      appendf(out, max_len, off, "pdu[%zu].rsrp=%u\n", i, up.rsrp);
      int sr_detected = 0;
      int sr_conf     = 0;
      if (up.sr.has_value()) {
        sr_detected = up.sr->sr_detected ? 1 : 0;
        sr_conf     = 1;
      }
      appendf(out, max_len, off, "pdu[%zu].sr_indication=%d\n", i, sr_detected);
      appendf(out, max_len, off, "pdu[%zu].sr_confidence_level=%d\n", i, sr_conf);
      int harq_val = 0;
      if (up.harq.has_value() && !up.harq->harq_values.empty()) {
        harq_val = to_int(up.harq->harq_values[0]);
      }
      appendf(out, max_len, off, "pdu[%zu].harq[0].harq_value=%d\n", i, harq_val);
      appendf(out, max_len, off, "pdu[%zu].harq_confidence_level=0\n", i);
    } else {
      const auto& up = std::get<fapi::uci_pucch_pdu_format_2_3_4>(p);
      appendf(out, max_len, off, "pdu[%zu].pdu_type=2\n", i); // 2 = PUCCH F2/3/4
      appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, up.handle);
      appendf(out, max_len, off, "pdu[%zu].rnti=%u\n", i, static_cast<unsigned>(up.rnti));
      appendf(out, max_len, off, "pdu[%zu].pucch_format=%d\n", i, to_int(up.pucch_format));
      appendf(out, max_len, off, "pdu[%zu].ul_sinr_metric=%d\n", i, up.ul_sinr_metric);
      appendf(out, max_len, off, "pdu[%zu].rssi=%u\n", i, up.rssi);
      appendf(out, max_len, off, "pdu[%zu].rsrp=%u\n", i, up.rsrp);
      appendf(out, max_len, off, "pdu[%zu].sr_indication=%d\n", i,
              static_cast<int>(up.sr.has_value()));
      appendf(out, max_len, off, "pdu[%zu].harq[0].harq_value=%d\n", i,
              static_cast<int>(up.harq.has_value()));
    }
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_srs_indication(char* out, int max_len, const fapi::srs_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=SRS_INDICATION\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 5;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, p.handle);
    appendf(out, max_len, off, "pdu[%zu].rnti=%u\n", i, static_cast<unsigned>(p.rnti));
    appendf(out, max_len, off, "pdu[%zu].has_matrix=%d\n", i, static_cast<int>(p.matrix.has_value()));
    appendf(out, max_len, off, "pdu[%zu].has_positioning=%d\n", i,
            static_cast<int>(p.positioning.has_value()));
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_rach_indication(char* out, int max_len, const fapi::rach_indication& msg)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=RACH_INDICATION\n");
  appendf(out, max_len, off, "SFN=%u\n", msg.slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", msg.slot.slot_index());
  appendf(out, max_len, off, "n_pdus=%zu\n", msg.pdus.size());

  const size_t pdu_limit = 3;
  for (size_t i = 0; i < msg.pdus.size() && i < pdu_limit; ++i) {
    const auto& p = msg.pdus[i];
    appendf(out, max_len, off, "pdu[%zu].handle=%u\n", i, p.handle);
    appendf(out, max_len, off, "pdu[%zu].symbol_index=%u\n", i, p.symbol_index);
    appendf(out, max_len, off, "pdu[%zu].slot_index=%u\n", i, p.slot_index);
    appendf(out, max_len, off, "pdu[%zu].ra_index=%u\n", i, p.ra_index);
    appendf(out, max_len, off, "pdu[%zu].avg_rssi=%u\n", i, p.avg_rssi);
    appendf(out, max_len, off, "pdu[%zu].avg_snr=%u\n", i, p.avg_snr);
    appendf(out, max_len, off, "pdu[%zu].num_preamble=%zu\n", i, p.preambles.size());
    const size_t pre_limit = 2;
    for (size_t j = 0; j < p.preambles.size() && j < pre_limit; ++j) {
      const auto& pre = p.preambles[j];
      appendf(out, max_len, off, "pdu[%zu].preamble[%zu].preamble_index=%u\n", i, j, pre.preamble_index);
      appendf(out, max_len, off, "pdu[%zu].preamble[%zu].preamble_pwr=%u\n", i, j, pre.preamble_pwr);
      appendf(out, max_len, off, "pdu[%zu].preamble[%zu].preamble_snr=%u\n", i, j, pre.preamble_snr);
    }
  }
  if (msg.pdus.size() > pdu_limit) {
    appendf(out, max_len, off, "...(showing first %zu of %zu PDUs)\n", pdu_limit, msg.pdus.size());
  }
  return static_cast<int>(msg.pdus.size());
}

int serialize_last_message(char* out, int max_len, slot_point slot)
{
  int off = 0;
  appendf(out, max_len, off, "msg_type=P7_LAST_MESSAGE\n");
  appendf(out, max_len, off, "SFN=%u\n", slot.sfn());
  appendf(out, max_len, off, "Slot=%u\n", slot.slot_index());
  return 0;
}

} // namespace ocudu::fapi_stats
