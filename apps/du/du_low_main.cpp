#include "apps/du_low/du_low_appconfig.h"
#include "apps/du_low/du_low_appconfig_cli11_schema.h"
#include "apps/du_low/du_low_appconfig_translators.h"
#include "apps/du_low/du_low_appconfig_validators.h"
#include "apps/du_low/du_low_appconfig_yaml_writer.h"
#include "apps/helpers/metrics/metrics_helpers.h"
#include "apps/services/application_message_banners.h"
#include "apps/services/metrics/metrics_notifier_proxy.h"
#include "apps/services/worker_manager/worker_manager.h"
#include "apps/units/application_unit.h"
#include "apps/units/flexible_o_du/split_6/o_du_low/split6_o_du_low_application_unit_impl.h"
#include "fapi_stats.h"
#include "ocudu/support/fapi_split_trace.h"
#include "fapi_xsm_transport.h"
#include "ocudu/adt/scope_exit.h"
#include "ocudu/support/backtrace.h"
#include "ocudu/support/config_parsers.h"
#include "ocudu/support/cpu_features.h"
#include "ocudu/support/signal_handling.h"
#include "ocudu/support/signal_observer.h"
#include "ocudu/support/sysinfo.h"
#include "ocudu/support/versioning/build_info.h"
#include "ocudu/support/versioning/version.h"
#include "ocudu/support/tsan_options.h"

#include <atomic>
#include <chrono>
#include <thread>

#ifdef DPDK_FOUND
#include "ocudu/hal/dpdk/dpdk_eal_factory.h"
#include <rte_ethdev.h>
#endif

using namespace ocudu;

namespace ocudu {
extern fapi_xsm_transport* g_l1_xsm_transport;
} // namespace ocudu


static std::string              config_file;
static std::atomic<bool>        is_app_running{true};
static constexpr unsigned       MAX_CONFIG_FILES = 10;
static signal_dispatcher        cleanup_signal_dispatcher;

static void populate_cli11_generic_args(CLI::App& app)
{
  fmt::memory_buffer buffer;
  fmt::format_to(std::back_inserter(buffer), "OCUDU 5G DU low (FAPI split / xSM) version {} ({})",
                 get_version(), get_build_hash());
  app.set_version_flag("-v,--version", ocudu::to_c_str(buffer));
  app.set_config("-c,", config_file, "Read config from file", false)->expected(1, MAX_CONFIG_FILES);
}

static void interrupt_signal_handler(int /*signal*/)
{
  is_app_running = false;
}

static void cleanup_signal_handler(int signal)
{
  cleanup_signal_dispatcher.notify_signal(signal);
  ocudulog::flush();
}

static void app_error_report_handler()
{
  ocudulog::flush();
}

static void initialize_log(const std::string& filename)
{
  ocudulog::sink* log_sink =
      (filename == "stdout") ? ocudulog::create_stdout_sink() : ocudulog::create_file_sink(filename);
  if (log_sink == nullptr) {
    report_error("Could not create application main log sink.\n");
  }
  ocudulog::set_default_sink(*log_sink);
  ocudulog::init();
}

static void register_app_logs(const du_low_appconfig& du_cfg, application_unit& du_low_app_unit)
{
  const logger_appconfig& log_cfg = du_cfg.log_cfg;

  auto& logger = ocudulog::fetch_basic_logger("ALL", false);
  logger.set_level(log_cfg.lib_level);
  logger.set_hex_dump_max_size(log_cfg.hex_max_size);

  auto& app_logger = ocudulog::fetch_basic_logger("APP", false);
  app_logger.set_level(ocudulog::basic_levels::info);
  app_services::application_message_banners::log_build_info(app_logger);
  app_logger.set_level(log_cfg.all_level);
  app_logger.set_hex_dump_max_size(log_cfg.hex_max_size);

  auto& config_logger = ocudulog::fetch_basic_logger("CONFIG", false);
  config_logger.set_level(log_cfg.config_level);
  config_logger.set_hex_dump_max_size(log_cfg.hex_max_size);

  const app_helpers::metrics_config& metrics_cfg = du_cfg.metrics_cfg.rusage_config.metrics_consumers_cfg;
  app_helpers::initialize_metrics_log_channels(metrics_cfg, log_cfg.hex_max_size);

  du_low_app_unit.on_loggers_registration();
}

int main(int argc, char** argv)
{
  set_error_handler(app_error_report_handler);

  static constexpr std::string_view app_name = "DU low (xSM)";
  app_services::application_message_banners::announce_app_and_version(app_name);

  register_interrupt_signal_handler(interrupt_signal_handler);
  register_cleanup_signal_handler(cleanup_signal_handler);
  enable_backtrace();

  CLI::App app("OCUDU DU low (FAPI split over xSM)");
  app.config_formatter(create_yaml_config_parser());
  app.allow_config_extras(CLI::config_extras_mode::error);
  populate_cli11_generic_args(app);

  du_low_appconfig du_low_cfg;
  configure_cli11_with_du_low_appconfig_schema(app, du_low_cfg);

  auto o_du_app_unit = create_flexible_o_du_low_application_unit(app_name);
  o_du_app_unit->on_parsing_configuration_registration(app);

  app.callback([&app, &du_low_cfg, &o_du_app_unit]() {
    autoderive_du_low_parameters_after_parsing(app, du_low_cfg);
    o_du_app_unit->on_configuration_parameters_autoderivation(app);
  });

  CLI11_PARSE(app, argc, argv);

  if (du_low_cfg.enable_dryrun) {
    return 0;
  }

  if (!validate_du_low_appconfig(du_low_cfg) || !o_du_app_unit->on_configuration_validation()) {
    report_error("Invalid configuration detected.\n");
  }

  initialize_log(du_low_cfg.log_cfg.filename);
  auto log_flusher = make_scope_exit([]() { ocudulog::flush(); });
  register_app_logs(du_low_cfg, *o_du_app_unit);


  ocudu::fapi_stats::initialize(du_low_cfg.fapi_stats_cfg.enabled,
                                du_low_cfg.fapi_stats_cfg.output_path,
                                du_low_cfg.fapi_stats_cfg.add_timestamp);


  ocudu::fapi_split_trace::init(du_low_cfg.fapi_stats_cfg.enabled,
                                "./logs/fapi_split_trace.log",
                                "odu_low");
  ocudu::fapi_split_trace::event("LIFECYCLE",
                                 "odu_low process started, fapi_stats.enabled=%d",
                                 du_low_cfg.fapi_stats_cfg.enabled ? 1 : 0);

  ocudulog::basic_logger& app_logger = ocudulog::fetch_basic_logger("APP");

  ocudulog::basic_logger& config_logger = ocudulog::fetch_basic_logger("CONFIG");
  if (config_logger.debug.enabled()) {
    YAML::Node node;
    fill_du_low_appconfig_in_yaml_schema(node, du_low_cfg);
    o_du_app_unit->dump_config(node);
    config_logger.debug("Input configuration (all values):\n{}", YAML::Dump(node));
  } else {
    config_logger.info("Input configuration (only non-default values):\n{}", app.config_to_str(false, false));
  }

  cpu_architecture_info::get().print_cpu_info(app_logger);
  if (cpu_supports_included_features()) {
    app_logger.debug("Required CPU features: {}", get_cpu_feature_info());
  } else {
    app_logger.error("The CPU does not support required CPU features: {}", get_cpu_feature_info());
    report_error("The CPU does not support required CPU features: {}\n", get_cpu_feature_info());
  }
  check_cpu_governor(app_logger);
  check_drm_kms_polling(app_logger);


#ifdef DPDK_FOUND
  std::unique_ptr<dpdk::dpdk_eal> eal;
  if (!du_low_cfg.hal_config) {
    report_error("odu_low requires a 'hal:' section with eal_args (DPDK + NIC binding) "
                 "in its YAML config. See configs/odu_low.yaml.\n");
  }
  {
    std::string eal_args = std::string(argv[0]) + " " + du_low_cfg.hal_config->eal_args
                           + " --file-prefix=gnb0 --proc-type="
                           + du_low_cfg.fapi_split_l1_cfg.dpdk_proc_type;
    eal = dpdk::create_dpdk_eal(eal_args, ocudulog::fetch_basic_logger("EAL", false));
  }

  if (::rte_eth_dev_count_avail() == 0) {
    report_error("DPDK EAL initialized but found 0 Ethernet ports.\n"
                 "Bind the NIC to a DPDK-compatible driver before starting odu_low:\n"
                 "  dpdk-devbind.py --bind=vfio-pci 0000:51:0a.0\n");
  }
#else
  report_error("odu_low requires DPDK_FOUND — rebuild with DPDK enabled.\n");
#endif


  fapi_xsm_transport xsm_transport;
  xsm_context&       xsm = xsm_transport.get_xsm_context();

  constexpr uint64_t mac_mem_size      = 2346713088ULL; // ~2.2 GB
  constexpr uint64_t phy_mem_size      = 0;
  const bool         dpdk_is_secondary = (du_low_cfg.fapi_split_l1_cfg.dpdk_proc_type == "secondary");
  const bool         is_master         = dpdk_is_secondary;
  const std::string& xsm_device        = du_low_cfg.fapi_split_l1_cfg.xsm_device_name;
  const uint32_t     pair_index        = du_low_cfg.fapi_split_l1_cfg.xsm_pair_index;
  const uint32_t     num_pairs         = du_low_cfg.fapi_split_l1_cfg.xsm_num_pairs;
  if (!xsm.open(xsm_device.c_str(), is_master, mac_mem_size, phy_mem_size, pair_index, num_pairs)) {
    report_error("XSM_Open failed (device={}, role={}, pair={}, num_pairs={})\n",
                 xsm_device, is_master ? "MASTER" : "SLAVE", pair_index, num_pairs);
  }

  app_logger.info("[xSM] device=\"{}\" role={} dpdk_proc_type={} — waiting for peer for up to 60s...",
                  xsm_device,
                  is_master ? "MASTER" : "SLAVE",
                  du_low_cfg.fapi_split_l1_cfg.dpdk_proc_type);
  if (!xsm.wait_for_peer(std::chrono::milliseconds(60000))) {
    xsm.close();
    report_error("xSM peer did not connect within 60s (device={})\n", xsm_device);
  }
  xsm_transport.init_l1_side();


  ocudu::g_l1_xsm_transport = &xsm_transport;

  timer_manager app_timers{256};
  app_services::metrics_notifier_proxy_impl metrics_notifier_forwarder;

  worker_manager_config worker_manager_cfg;
  fill_du_low_worker_manager_config(worker_manager_cfg, du_low_cfg);
  o_du_app_unit->fill_worker_manager_config(worker_manager_cfg);
  worker_manager_cfg.app_timers = &app_timers;

  worker_manager workers{worker_manager_cfg};


  auto du = o_du_app_unit->create_flexible_o_du_low(
      workers, metrics_notifier_forwarder, /*remote_metrics_gateway=*/nullptr, app_timers, app_logger);

  if (!du.odu_low) {
    report_error("create_flexible_o_du_low returned null\n");
  }



  xsm_transport.set_rx_cpu(du_low_cfg.fapi_split_l1_cfg.rx_cpu);
  xsm_transport.set_rx_priority(du_low_cfg.fapi_split_l1_cfg.rx_priority);

  xsm_transport.start_receiver();


  du.odu_low->start();
  app_logger.info("O-DU low started — waiting for P5 sequence from L2 over xSM");

  {
    app_services::application_message_banners app_banner(app_name, du_low_cfg.log_cfg.filename);
    while (is_app_running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }

  app_logger.info("Shutdown signal received");

  du.odu_low->stop();
  xsm_transport.shutdown();
  ocudu::g_l1_xsm_transport = nullptr;

  ocudu::fapi_stats::dump_to_json();
  ocudu::fapi_stats::shutdown();

  ocudu::fapi_split_trace::event("LIFECYCLE", "odu_low process shutting down");
  ocudu::fapi_split_trace::shutdown();

  app_logger.info("O-DU low terminated");
  return 0;
}
