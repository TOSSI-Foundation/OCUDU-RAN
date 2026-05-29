// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ru_emulator_cli11_schema.h"
#include "helpers.h"
#include "ru_emulator_appconfig.h"
#include "ocudu/support/cli11_utils.h"
#include "ocudu/support/config_parsers.h"

using namespace ocudu;

/// Translates a string to the corresponding RU emulator's PRACH format.
static ru_emulator_prach_format str_to_prach_format(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), ::toupper);

  if ("LONG" == s) {
    return ru_emulator_prach_format::LONG_F0;
  }
  if ("SHORT" == s) {
    return ru_emulator_prach_format::SHORT_B4;
  }
  return ru_emulator_prach_format::NONE;
}

/// Translates RU emulator's PRACH format to a string.
static std::string prach_format_to_str(ru_emulator_prach_format f)
{
  if (f == ru_emulator_prach_format::LONG_F0) {
    return "long";
  }
  if (f == ru_emulator_prach_format::SHORT_B4) {
    return "short";
  }
  return "";
}

static void configure_cli11_log_args(CLI::App& app, ru_emulator_log_appconfig& log_params)
{
  /// Function to check that the log level is correct.
  auto check_log_level = [](const std::string& value) -> std::string {
    if (ocudulog::str_to_basic_level(value).has_value()) {
      return {};
    }

    return fmt::format("Log level '{}' not supported. Accepted values [none,info,debug,warning,error]", value);
  };
  /// Function to convert string parameter to ocudulog level.
  auto capture_log_level_function = [](ocudulog::basic_levels& level) {
    return [&level](const std::string& value) {
      auto val = ocudulog::str_to_basic_level(value);
      level    = (val) ? val.value() : ocudulog::basic_levels::none;
    };
  };

  app.add_option("--filename", log_params.filename, "Log file output path")->capture_default_str();
  add_option_function<std::string>(app, " --level", capture_log_level_function(log_params.level), "Log level")
      ->default_str(ocudulog::basic_level_to_string(log_params.level))
      ->check(check_log_level);
}

static void configure_cli11_ru_emu_dpdk_args(CLI::App& app, std::optional<ru_emulator_dpdk_appconfig>& config)
{
  config.emplace();

  app.add_option("--eal_args", config->eal_args, "EAL configuration parameters used to initialize DPDK");
}

static void configure_cli11_ru_emu_args(CLI::App& app, ru_emulator_ofh_appconfig& config)
{
  app.add_option_function<unsigned>(
         "--bandwidth",
         [&config](unsigned value) { config.bandwidth = MHz_to_bs_channel_bandwidth(value); },
         "Channel bandwidth in MHz")
      ->check([](const std::string& value) -> std::string {
        std::stringstream ss(value);
        unsigned          bw;
        ss >> bw;
        const std::string& error_message = "Error in the channel bandwidth property. Valid values "
                                           "[5,10,15,20,25,30,40,50,60,70,80,90,100]";

        return is_valid_bw(bw) ? "" : error_message;
      });

  auto compression_method_check = [](const std::string& value) -> std::string {
    if (value == "none" || value == "bfp") {
      return {};
    }

    return "Compression method not supported. Accepted values [none, bfp]";
  };

  app.add_option("--compr_method_ul", config.ul_compr_method, "Uplink compression method")
      ->capture_default_str()
      ->check(compression_method_check);
  app.add_option("--compr_bitwidth_ul", config.ul_compr_bitwidth, "Uplink compression bit width")
      ->capture_default_str()
      ->check(CLI::IsMember({9, 16}));
  app.add_option("--network_interface", config.network_interface, "PCIe identifier of network device")
      ->capture_default_str();
  app.add_option("--ru_mac_addr", config.ru_mac_address, "Radio Unit MAC address")->capture_default_str();
  app.add_option("--du_mac_addr", config.du_mac_address, "Distributed Unit MAC address")->capture_default_str();
  app.add_option("--vlan_tag", config.vlan_tag, "V-LAN identifier")->capture_default_str()->check(CLI::Range(1, 4094));
  app.add_option("--enable_promiscuous", config.enable_promiscuous, "Promiscuous mode flag")->capture_default_str();
  app.add_option("--ul_port_id", config.ru_ul_port_id, "RU uplink port identifier")->capture_default_str();
  app.add_option("--dl_port_id", config.ru_dl_port_id, "RU downlink port identifier")->capture_default_str();
  app.add_option("--prach_port_id", config.ru_prach_port_id, "RU PRACH port identifier")->capture_default_str();

  // Note: For the timing parameters, worst case is 2 slots for scs 15KHz and 14 symbols. Implementation defined.
  app.add_option("--t2a_max_cp_dl", config.T2a_max_cp_dl, "T2a maximum value for downlink Control-Plane")
      ->capture_default_str()
      ->check(CLI::Range(0, 1960));
  app.add_option("--t2a_min_cp_dl", config.T2a_min_cp_dl, "T2a minimum value for downlink Control-Plane")
      ->capture_default_str()
      ->check(CLI::Range(0, 1960));
  app.add_option("--t2a_max_cp_ul", config.T2a_max_cp_ul, "T2a maximum value for uplink Control-Plane")
      ->capture_default_str()
      ->check(CLI::Range(0, 1960));
  app.add_option("--t2a_min_cp_ul", config.T2a_min_cp_ul, "T2a minimum value for uplink Control-Plane")
      ->capture_default_str()
      ->check(CLI::Range(0, 1960));
  app.add_option("--t2a_max_up", config.T2a_max_up, "T2a maximum value for User-Plane")
      ->capture_default_str()
      ->check(CLI::Range(0, 1960));
  app.add_option("--t2a_min_up", config.T2a_min_up, "T2a minimum value for User-Plane")
      ->capture_default_str()
      ->check(CLI::Range(0, 1960));

  // Function to capture the PRACH format.
  auto capture_prach_format_function = [](ru_emulator_prach_format& format) {
    return [&format](const std::string& value) { format = str_to_prach_format(value); };
  };

  // Function to check that the log level is correct.
  auto check_prach_format = [](const std::string& value) -> std::string {
    if (str_to_prach_format(value) != ocudu::ru_emulator_prach_format::NONE) {
      return {};
    }
    return fmt::format("PRACH format '{}' not supported. Accepted values [long,short]. Set to 'long' to use format 0, "
                       "or 'short' to use format B4",
                       value);
  };

  add_option_function<std::string>(app,
                                   "--prach_format",
                                   capture_prach_format_function(config.prach_format),
                                   "PRACH format. Set to 'long' to use format 0, or 'short' to use format B4")
      ->default_str(prach_format_to_str(config.prach_format))
      ->check(check_prach_format);

  add_option(app,
             "--prach_inject_real_preamble",
             config.prach_inject_real_preamble,
             "If true, fill PRACH U-Plane packets with a real frequency-domain Zadoff-Chu preamble "
             "generated by ocudu's prach_generator (per 3GPP TS 38.211 §6.3.3) instead of random bytes")
      ->capture_default_str();
  add_option(app,
             "--prach_root_sequence_index",
             config.prach_root_sequence_index,
             "Zadoff-Chu root sequence index (i). MUST match the DU's prach_root_sequence_index")
      ->capture_default_str();
  add_option(app,
             "--prach_zero_correlation_zone",
             config.prach_zero_correlation_zone,
             "Cyclic shift configuration index. MUST match the DU's zero_correlation_zone")
      ->capture_default_str()
      ->check(CLI::Range(0u, 15u));
  add_option(app,
             "--prach_preamble_index",
             config.prach_preamble_index,
             "Preamble index 0..63 to emit on every PRACH occasion (ignored when round_robin is true)")
      ->capture_default_str()
      ->check(CLI::Range(0u, 63u));
  add_option(app,
             "--prach_round_robin",
             config.prach_round_robin,
             "If true, cycle preamble_index 0..63 across PRACH occasions instead of using a fixed index")
      ->capture_default_str();
  add_option(app,
             "--prach_start_prb",
             config.prach_start_prb,
             "First PRB of the PRACH IQ block in the carrier grid. MUST equal the DU cell's "
             "prach.prach_frequency_start or the DU writer drops the section as out-of-band")
      ->capture_default_str()
      ->check(CLI::Range(0u, 274u));
}

void ocudu::configure_cli11_with_ru_emulator_appconfig_schema(CLI::App& app, ru_emulator_appconfig& ru_emu_parsed_cfg)
{
  // Logging section.
  CLI::App* log_subcmd = app.add_subcommand("log", "Logging configuration")->configurable();
  configure_cli11_log_args(*log_subcmd, ru_emu_parsed_cfg.log_cfg);

  // RU emulators section.
  CLI::App* ru_subcmd =
      app.add_subcommand("ru_emu", "Open Fronthaul Radio Unit emulator configuration")->configurable();

  // Cell parameters.
  ru_subcmd->add_option_function<std::vector<std::string>>(
      "--cells",
      [&ru_emu_parsed_cfg](const std::vector<std::string>& values) {
        ru_emu_parsed_cfg.ru_cfg.resize(values.size());

        for (unsigned i = 0, e = values.size(); i != e; ++i) {
          CLI::App subapp("RU emulators");
          subapp.config_formatter(create_yaml_config_parser());
          subapp.allow_config_extras(CLI::config_extras_mode::error);
          configure_cli11_ru_emu_args(subapp, ru_emu_parsed_cfg.ru_cfg[i]);
          std::istringstream ss(values[i]);
          subapp.parse_from_stream(ss);
        }
      },
      "Sets the RU emulator configuration");

  CLI::App* dpdk_subcmd = app.add_subcommand("dpdk", "DPDK configuration")->configurable();
  configure_cli11_ru_emu_dpdk_args(*dpdk_subcmd, ru_emu_parsed_cfg.dpdk_config);

  app.callback([&]() {
    // Clean the DPDK optional.
    if (app.get_subcommand("dpdk")->count_all() == 0) {
      ru_emu_parsed_cfg.dpdk_config.reset();
    }
#ifdef DPDK_FOUND
    bool uses_dpdk = ru_emu_parsed_cfg.dpdk_config.has_value();
    if (uses_dpdk && ru_emu_parsed_cfg.dpdk_config->eal_args.empty()) {
      report_error("It is mandatory to fill the EAL configuration arguments to initialize DPDK correctly");
    }
#else
    if (ru_emu_parsed_cfg.dpdk_config.has_value()) {
      report_error("Unable to use DPDK as the application was not compiled with DPDK support");
    }
#endif
  });
}
