#pragma once

#include "ocudu/channel/sionna/cir_artifact.h"
#include <atomic>
#include <memory>

namespace ocudu {

class cir_source
{
public:
  virtual ~cir_source() = default;

  virtual std::shared_ptr<const cir_artifact> get_artifact() = 0;
};

class static_cir_source : public cir_source
{
public:
  explicit static_cir_source(std::shared_ptr<const cir_artifact> artifact_) : artifact(std::move(artifact_)) {}

  std::shared_ptr<const cir_artifact> get_artifact() override { return artifact; }

private:
  std::shared_ptr<const cir_artifact> artifact;
};


class swappable_cir_source : public cir_source
{
public:
  explicit swappable_cir_source(std::shared_ptr<const cir_artifact> artifact = nullptr) :
    current(std::move(artifact))
  {
  }

  std::shared_ptr<const cir_artifact> get_artifact() override
  {
    return std::atomic_load_explicit(&current, std::memory_order_acquire);
  }

  void publish(std::shared_ptr<const cir_artifact> artifact)
  {
    std::atomic_store_explicit(&current, std::move(artifact), std::memory_order_release);
    ++updates;
  }

  uint64_t get_nof_updates() const { return updates.load(std::memory_order_relaxed); }

private:
  std::shared_ptr<const cir_artifact> current;
  std::atomic<uint64_t>               updates = {0};
};

} // namespace ocudu
