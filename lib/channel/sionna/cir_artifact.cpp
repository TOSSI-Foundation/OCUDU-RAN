#include "ocudu/channel/sionna/cir_artifact.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace ocudu;

static constexpr char     expected_format[]  = "ocudu-sionna-cir";
static constexpr unsigned expected_version   = 1;
static constexpr unsigned max_taps           = 64;
static constexpr unsigned max_antennas       = 4;
static constexpr unsigned max_snapshots      = 1000000;

expected<cir_artifact, std::string> ocudu::load_cir_artifact(const std::string& manifest_path)
{
  std::ifstream manifest_stream(manifest_path);
  if (!manifest_stream.is_open()) {
    return make_unexpected(fmt::format("Cannot open CIR manifest '{}'.", manifest_path));
  }

  nlohmann::json manifest = nlohmann::json::parse(manifest_stream, nullptr, false);
  if (manifest.is_discarded()) {
    return make_unexpected(fmt::format("CIR manifest '{}' is not valid JSON.", manifest_path));
  }

  auto require = [&manifest](const char* key) -> bool { return manifest.contains(key); };
  for (const char* key : {"format", "version", "fs_hz", "num_snapshots", "num_tx_ant", "num_rx_ant", "num_taps",
                          "normalization", "data_file"}) {
    if (!require(key)) {
      return make_unexpected(fmt::format("CIR manifest is missing required field '{}'.", key));
    }
  }

  if (manifest["format"].get<std::string>() != expected_format) {
    return make_unexpected(
        fmt::format("Unsupported CIR manifest format '{}', expected '{}'.", manifest["format"].get<std::string>(), expected_format));
  }
  if (manifest["version"].get<unsigned>() != expected_version) {
    return make_unexpected(
        fmt::format("Unsupported CIR manifest version {}, expected {}.", manifest["version"].get<unsigned>(), expected_version));
  }

  cir_artifact art;
  art.fs_hz         = manifest["fs_hz"].get<double>();
  art.nof_snapshots = manifest["num_snapshots"].get<unsigned>();
  art.nof_tx_ant    = manifest["num_tx_ant"].get<unsigned>();
  art.nof_rx_ant    = manifest["num_rx_ant"].get<unsigned>();
  art.nof_taps      = manifest["num_taps"].get<unsigned>();
  art.normalization = manifest["normalization"].get<std::string>();
  art.snapshot_dt_s = manifest.value("snapshot_dt_s", 0.0);
  art.loop          = manifest.value("loop", false);
  art.scene         = manifest.value("scene", std::string());

  if (!std::isfinite(art.fs_hz) || (art.fs_hz <= 0.0)) {
    return make_unexpected(fmt::format("CIR manifest fs_hz {} is not a valid sampling rate.", art.fs_hz));
  }
  if ((art.nof_snapshots == 0) || (art.nof_snapshots > max_snapshots)) {
    return make_unexpected(fmt::format("CIR manifest num_snapshots {} out of range [1, {}].", art.nof_snapshots, max_snapshots));
  }
  if ((art.nof_tx_ant == 0) || (art.nof_tx_ant > max_antennas) || (art.nof_rx_ant == 0) ||
      (art.nof_rx_ant > max_antennas)) {
    return make_unexpected(fmt::format(
        "CIR manifest antenna counts {}x{} out of range [1, {}].", art.nof_tx_ant, art.nof_rx_ant, max_antennas));
  }
  if ((art.nof_taps == 0) || (art.nof_taps > max_taps)) {
    return make_unexpected(fmt::format("CIR manifest num_taps {} out of range [1, {}].", art.nof_taps, max_taps));
  }
  if ((art.nof_snapshots > 1) && (!std::isfinite(art.snapshot_dt_s) || (art.snapshot_dt_s <= 0.0))) {
    return make_unexpected(
        fmt::format("CIR manifest snapshot_dt_s {} must be positive for multi-snapshot artifacts.", art.snapshot_dt_s));
  }
  if ((art.normalization != "unit_energy") && (art.normalization != "absolute")) {
    return make_unexpected(
        fmt::format("CIR manifest normalization '{}' must be 'unit_energy' or 'absolute'.", art.normalization));
  }

  std::filesystem::path data_path = manifest["data_file"].get<std::string>();
  if (data_path.is_relative()) {
    data_path = std::filesystem::path(manifest_path).parent_path() / data_path;
  }

  std::error_code ec;
  uintmax_t       file_size = std::filesystem::file_size(data_path, ec);
  if (ec) {
    return make_unexpected(fmt::format("Cannot access CIR data file '{}': {}.", data_path.string(), ec.message()));
  }

  size_t nof_values     = static_cast<size_t>(art.nof_snapshots) * art.nof_rx_ant * art.nof_tx_ant * art.nof_taps;
  size_t expected_bytes = nof_values * sizeof(cf_t);
  if (file_size != expected_bytes) {
    return make_unexpected(fmt::format(
        "CIR data file '{}' size {} does not match manifest geometry ({} bytes expected).",
        data_path.string(),
        file_size,
        expected_bytes));
  }

  art.taps.resize(nof_values);
  std::ifstream data_stream(data_path, std::ios::binary);
  if (!data_stream.is_open() ||
      !data_stream.read(reinterpret_cast<char*>(art.taps.data()), static_cast<std::streamsize>(expected_bytes))) {
    return make_unexpected(fmt::format("Failed to read CIR data file '{}'.", data_path.string()));
  }

  for (const cf_t& tap : art.taps) {
    if (!std::isfinite(tap.real()) || !std::isfinite(tap.imag())) {
      return make_unexpected(fmt::format("CIR data file '{}' contains non-finite tap values.", data_path.string()));
    }
  }

  return art;
}
