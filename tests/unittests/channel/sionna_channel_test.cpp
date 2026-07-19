

#include "ocudu/channel/sionna/cir_artifact.h"
#include "ocudu/channel/sionna/cir_source.h"
#include "ocudu/channel/sionna/cir_zmq_receiver.h"
#include "ocudu/channel/sionna/sionna_channel_engine.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <gtest/gtest.h>
#include <random>

using namespace ocudu;

namespace {

constexpr float assert_tolerance = 1e-5F;

std::shared_ptr<cir_artifact> make_artifact(std::vector<std::vector<cf_t>> snapshots,
                                            double                         fs_hz         = 61.44e6,
                                            double                         snapshot_dt_s = 0.0,
                                            bool                           loop          = false)
{
  auto art           = std::make_shared<cir_artifact>();
  art->fs_hz         = fs_hz;
  art->snapshot_dt_s = snapshot_dt_s;
  art->nof_snapshots = snapshots.size();
  art->nof_tx_ant    = 1;
  art->nof_rx_ant    = 1;
  art->nof_taps      = snapshots.front().size();
  art->loop          = loop;
  art->normalization = "unit_energy";
  for (const auto& snap : snapshots) {
    art->taps.insert(art->taps.end(), snap.begin(), snap.end());
  }
  return art;
}

std::vector<cf_t> reference_fir(span<const cf_t> taps, span<const cf_t> input)
{
  std::vector<cf_t> out(input.size());
  for (unsigned n = 0; n != input.size(); ++n) {
    cf_t acc = 0;
    for (unsigned k = 0; k != taps.size(); ++k) {
      if (n >= k) {
        acc += taps[k] * input[n - k];
      }
    }
    out[n] = acc;
  }
  return out;
}

std::vector<cf_t> random_signal(unsigned nof_samples, unsigned seed)
{
  std::mt19937                          rgen(seed);
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  std::vector<cf_t>                     out(nof_samples);
  for (cf_t& sample : out) {
    sample = {dist(rgen), dist(rgen)};
  }
  return out;
}

void expect_near(span<const cf_t> expected, span<const cf_t> actual)
{
  ASSERT_EQ(expected.size(), actual.size());
  for (unsigned i = 0; i != expected.size(); ++i) {
    EXPECT_NEAR(expected[i].real(), actual[i].real(), assert_tolerance) << "sample " << i;
    EXPECT_NEAR(expected[i].imag(), actual[i].imag(), assert_tolerance) << "sample " << i;
  }
}

TEST(sionna_channel_engine, unit_tap_is_passthrough)
{
  auto art = make_artifact({{cf_t(1.0F, 0.0F), cf_t(), cf_t(), cf_t()}});

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 1024, 64);

  std::vector<cf_t> input = random_signal(512, 7);
  std::vector<cf_t> data  = input;
  engine.process(0, data, 0);

  expect_near(input, data);
}

TEST(sionna_channel_engine, impulse_yields_taps)
{
  std::vector<cf_t> taps = {{0.5F, 0.1F}, {-0.2F, 0.3F}, {0.0F, 0.0F}, {0.1F, -0.4F}};
  auto              art  = make_artifact({taps});

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 64, 64);

  std::vector<cf_t> data(16, cf_t());
  data[0] = cf_t(1.0F, 0.0F);
  engine.process(0, data, 0);

  for (unsigned i = 0; i != taps.size(); ++i) {
    EXPECT_NEAR(taps[i].real(), data[i].real(), assert_tolerance);
    EXPECT_NEAR(taps[i].imag(), data[i].imag(), assert_tolerance);
  }
  for (unsigned i = taps.size(); i != data.size(); ++i) {
    EXPECT_NEAR(0.0F, std::abs(data[i]), assert_tolerance);
  }
}

TEST(sionna_channel_engine, matches_reference_filter)
{
  std::vector<cf_t> taps = {{0.9F, 0.0F}, {0.1F, -0.2F}, {-0.05F, 0.02F}, {0.0F, 0.0F}, {0.3F, 0.3F}};
  auto              art  = make_artifact({taps});

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 2048, 64);

  std::vector<cf_t> input    = random_signal(1500, 11);
  std::vector<cf_t> expected = reference_fir(taps, input);
  std::vector<cf_t> data     = input;
  engine.process(0, data, 0);

  expect_near(expected, data);
}

TEST(sionna_channel_engine, block_splitting_is_transparent)
{
  std::vector<cf_t> taps = {{0.7F, 0.1F}, {0.2F, -0.3F}, {-0.1F, 0.05F}};
  auto              art  = make_artifact({taps});

  std::vector<cf_t> input    = random_signal(900, 23);
  std::vector<cf_t> expected = reference_fir(taps, input);

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 512, 64);
  std::vector<cf_t>     data = input;

  span<cf_t> view = data;
  engine.process(0, view.subspan(0, 300), 0);
  engine.process(0, view.subspan(300, 400), 300);
  engine.process(0, view.subspan(700, 200), 700);

  expect_near(expected, data);
}

TEST(sionna_channel_engine, timestamp_gap_behaves_as_zero_fill)
{
  std::vector<cf_t> taps = {{0.6F, 0.0F}, {0.3F, 0.1F}, {0.1F, -0.1F}};
  auto              art  = make_artifact({taps});

  unsigned gap = 1;

  std::vector<cf_t> first  = random_signal(200, 31);
  std::vector<cf_t> second = random_signal(200, 37);

  // Reference: the same stream with explicit zeros in the gap.
  std::vector<cf_t> padded = first;
  padded.insert(padded.end(), gap, cf_t());
  padded.insert(padded.end(), second.begin(), second.end());
  std::vector<cf_t> expected_full = reference_fir(taps, padded);

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 512, 64);
  std::vector<cf_t>     data_first = first;
  engine.process(0, data_first, 0);
  std::vector<cf_t> data_second = second;
  engine.process(0, data_second, first.size() + gap);

  expect_near(span<const cf_t>(expected_full).first(first.size()), data_first);
  expect_near(span<const cf_t>(expected_full).last(second.size()), data_second);
}

TEST(sionna_channel_engine, snapshot_timeline_selection)
{
  // Two snapshots of one tap each; 1000 samples per snapshot at fs=1e6, dt=1ms.
  auto art = make_artifact({{cf_t(1.0F, 0.0F)}, {cf_t(2.0F, 0.0F)}}, 1e6, 1e-3, false);

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 64, 64);

  EXPECT_EQ(0U, engine.get_snapshot_index(0));
  EXPECT_EQ(0U, engine.get_snapshot_index(999));
  EXPECT_EQ(1U, engine.get_snapshot_index(1000));
  // Clamped at the last snapshot when not looping.
  EXPECT_EQ(1U, engine.get_snapshot_index(5000));

  std::vector<cf_t> data(4, cf_t(1.0F, 0.0F));
  engine.process(0, data, 2000);
  for (const cf_t& sample : data) {
    EXPECT_NEAR(2.0F, sample.real(), assert_tolerance);
  }
}

TEST(sionna_channel_engine, snapshot_timeline_loops)
{
  auto art = make_artifact({{cf_t(1.0F, 0.0F)}, {cf_t(2.0F, 0.0F)}}, 1e6, 1e-3, true);

  sionna_channel_engine engine(std::make_shared<static_cir_source>(art), 1, 64, 64);

  EXPECT_EQ(0U, engine.get_snapshot_index(0));
  EXPECT_EQ(1U, engine.get_snapshot_index(1000));
  EXPECT_EQ(0U, engine.get_snapshot_index(2000));
  EXPECT_EQ(1U, engine.get_snapshot_index(3000));
}

class cir_artifact_loader_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    dir = std::filesystem::temp_directory_path() / "ocudu_cir_test";
    std::filesystem::create_directories(dir);
  }

  void TearDown() override { std::filesystem::remove_all(dir); }

  void write_manifest(const std::string& contents)
  {
    std::ofstream out(dir / "manifest.json");
    out << contents;
  }

  void write_data(const std::vector<cf_t>& taps)
  {
    std::ofstream out(dir / "cir.bin", std::ios::binary);
    out.write(reinterpret_cast<const char*>(taps.data()), taps.size() * sizeof(cf_t));
  }

  std::string manifest_path() const { return (dir / "manifest.json").string(); }

  std::filesystem::path dir;
};

TEST_F(cir_artifact_loader_test, loads_valid_artifact)
{
  write_manifest(R"({
    "format": "ocudu-sionna-cir", "version": 1, "fs_hz": 61440000.0,
    "num_snapshots": 2, "num_tx_ant": 1, "num_rx_ant": 1, "num_taps": 2,
    "snapshot_dt_s": 0.1, "normalization": "unit_energy", "data_file": "cir.bin",
    "scene": "test"})");
  write_data({{1.0F, 0.0F}, {0.0F, 0.0F}, {0.5F, 0.5F}, {0.1F, 0.0F}});

  auto result = load_cir_artifact(manifest_path());
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(2U, result->nof_snapshots);
  EXPECT_EQ(2U, result->nof_taps);
  EXPECT_DOUBLE_EQ(61440000.0, result->fs_hz);
  EXPECT_NEAR(0.5F, result->get_taps(1, 0, 0)[0].real(), assert_tolerance);
}

TEST_F(cir_artifact_loader_test, rejects_missing_manifest)
{
  auto result = load_cir_artifact((dir / "does_not_exist.json").string());
  ASSERT_FALSE(result.has_value());
}

TEST_F(cir_artifact_loader_test, rejects_wrong_data_size)
{
  write_manifest(R"({
    "format": "ocudu-sionna-cir", "version": 1, "fs_hz": 61440000.0,
    "num_snapshots": 1, "num_tx_ant": 1, "num_rx_ant": 1, "num_taps": 4,
    "normalization": "unit_energy", "data_file": "cir.bin"})");
  write_data({{1.0F, 0.0F}});

  auto result = load_cir_artifact(manifest_path());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(std::string::npos, result.error().find("size"));
}

TEST_F(cir_artifact_loader_test, rejects_bad_format)
{
  write_manifest(R"({
    "format": "something-else", "version": 1, "fs_hz": 61440000.0,
    "num_snapshots": 1, "num_tx_ant": 1, "num_rx_ant": 1, "num_taps": 1,
    "normalization": "unit_energy", "data_file": "cir.bin"})");
  write_data({{1.0F, 0.0F}});

  auto result = load_cir_artifact(manifest_path());
  ASSERT_FALSE(result.has_value());
}

TEST_F(cir_artifact_loader_test, rejects_non_finite_taps)
{
  write_manifest(R"({
    "format": "ocudu-sionna-cir", "version": 1, "fs_hz": 61440000.0,
    "num_snapshots": 1, "num_tx_ant": 1, "num_rx_ant": 1, "num_taps": 1,
    "normalization": "unit_energy", "data_file": "cir.bin"})");
  write_data({{std::numeric_limits<float>::infinity(), 0.0F}});

  auto result = load_cir_artifact(manifest_path());
  ASSERT_FALSE(result.has_value());
}

TEST_F(cir_artifact_loader_test, rejects_missing_field)
{
  write_manifest(R"({
    "format": "ocudu-sionna-cir", "version": 1,
    "num_snapshots": 1, "num_tx_ant": 1, "num_rx_ant": 1, "num_taps": 1,
    "normalization": "unit_energy", "data_file": "cir.bin"})");
  write_data({{1.0F, 0.0F}});

  auto result = load_cir_artifact(manifest_path());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(std::string::npos, result.error().find("fs_hz"));
}

} // namespace

namespace {

/// Builds a live channel update message carrying a single-snapshot channel.
std::vector<uint8_t> make_update(span<const cf_t> taps,
                                 double           fs_hz         = 61.44e6,
                                 unsigned         num_tx_ant    = 1,
                                 unsigned         num_rx_ant    = 1,
                                 uint32_t         magic         = cir_update_magic,
                                 uint32_t         version       = cir_update_version)
{
  cir_update_header header = {};
  header.magic             = magic;
  header.version           = version;
  header.sequence          = 7;
  header.num_snapshots     = 1;
  header.num_tx_ant        = num_tx_ant;
  header.num_rx_ant        = num_rx_ant;
  header.num_taps          = taps.size() / (num_tx_ant * num_rx_ant);
  header.loop              = 0;
  header.fs_hz             = fs_hz;
  header.snapshot_dt_s     = 0.0;

  std::vector<uint8_t> message(sizeof(header) + taps.size() * sizeof(cf_t));
  std::memcpy(message.data(), &header, sizeof(header));
  std::memcpy(message.data() + sizeof(header), taps.data(), taps.size() * sizeof(cf_t));
  return message;
}

} // namespace

TEST(cir_live_update, decodes_valid_update)
{
  std::vector<cf_t> taps = {{0.5F, 0.25F}, {0.1F, 0.0F}, {}, {}};
  std::vector<uint8_t> msg = make_update(taps);

  auto decoded = decode_cir_update(msg, 61.44e6, 4, 4);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(1U, decoded->nof_snapshots);
  EXPECT_EQ(4U, decoded->nof_taps);
  EXPECT_EQ(1U, decoded->nof_tx_ant);
  EXPECT_EQ(61.44e6, decoded->fs_hz);
  EXPECT_EQ(taps[0], decoded->get_taps(0, 0, 0)[0]);
  EXPECT_EQ(taps[1], decoded->get_taps(0, 0, 0)[1]);
}

TEST(cir_live_update, rejects_wrong_sampling_rate)
{
  std::vector<cf_t> taps = {{1.0F, 0.0F}, {}};
  auto              res  = decode_cir_update(make_update(taps, 30.72e6), 61.44e6, 4, 4);
  EXPECT_FALSE(res.has_value());
}

TEST(cir_live_update, rejects_bad_magic)
{
  std::vector<cf_t> taps = {{1.0F, 0.0F}, {}};
  auto              res  = decode_cir_update(make_update(taps, 61.44e6, 1, 1, 0xdeadbeef), 61.44e6, 4, 4);
  EXPECT_FALSE(res.has_value());
}

TEST(cir_live_update, rejects_bad_version)
{
  std::vector<cf_t> taps = {{1.0F, 0.0F}, {}};
  auto res = decode_cir_update(make_update(taps, 61.44e6, 1, 1, cir_update_magic, 99), 61.44e6, 4, 4);
  EXPECT_FALSE(res.has_value());
}

TEST(cir_live_update, rejects_truncated_message)
{
  std::vector<cf_t>    taps = {{1.0F, 0.0F}, {}};
  std::vector<uint8_t> msg  = make_update(taps);
  msg.resize(msg.size() - sizeof(cf_t));
  EXPECT_FALSE(decode_cir_update(msg, 61.44e6, 4, 4).has_value());
}

TEST(cir_live_update, rejects_too_many_antennas)
{
  std::vector<cf_t> taps(8, cf_t(1.0F, 0.0F));
  auto              res = decode_cir_update(make_update(taps, 61.44e6, 4, 2), 61.44e6, 1, 1);
  EXPECT_FALSE(res.has_value());
}

TEST(sionna_channel_engine, live_swap_changes_the_applied_channel)
{
  // Start with a unit impulse: the output equals the input.
  auto unit  = make_artifact({{cf_t(1.0F, 0.0F), cf_t(), cf_t(), cf_t()}});
  auto half  = make_artifact({{cf_t(0.5F, 0.0F), cf_t(), cf_t(), cf_t()}});
  auto src   = std::make_shared<swappable_cir_source>(unit);
  sionna_channel_engine engine(src, 1, 256, 64);

  std::vector<cf_t> block(8, cf_t(1.0F, 0.0F));
  engine.process(0, block, 0);
  EXPECT_NEAR(1.0F, block[0].real(), 1e-6F);
  EXPECT_EQ(0U, engine.get_nof_artifact_changes());

  // Publish a new channel from another thread and confirm the next block uses it.
  src->publish(half);
  std::fill(block.begin(), block.end(), cf_t(1.0F, 0.0F));
  engine.process(0, block, 8);
  EXPECT_NEAR(0.5F, block[0].real(), 1e-6F);
  EXPECT_EQ(1U, engine.get_nof_artifact_changes());
}

TEST(sionna_channel_engine, passes_through_until_first_update)
{
  auto                  src = std::make_shared<swappable_cir_source>();
  sionna_channel_engine engine(src, 1, 256, 64);

  std::vector<cf_t> block = {{1.0F, 2.0F}, {3.0F, 4.0F}};
  std::vector<cf_t> ref   = block;
  engine.process(0, block, 0);
  EXPECT_EQ(ref, block);
}

TEST(sionna_channel_engine, rejects_artifact_exceeding_max_taps)
{
  auto small = make_artifact({{cf_t(1.0F, 0.0F), cf_t()}});
  auto src   = std::make_shared<swappable_cir_source>(small);
  // Engine preallocates for 2 taps only.
  sionna_channel_engine engine(src, 1, 256, 2);

  std::vector<cf_t> big_taps(8, cf_t(0.1F, 0.0F));
  src->publish(make_artifact({big_taps}));

  std::vector<cf_t> block(4, cf_t(1.0F, 0.0F));
  // Must not crash and must keep applying the previous 2-tap channel.
  engine.process(0, block, 0);
  EXPECT_NEAR(1.0F, block[0].real(), 1e-6F);
}
