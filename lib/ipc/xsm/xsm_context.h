#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace ocudu {

static constexpr uint32_t XSM_BLOCK_SIZE = 128 * 1024;

static constexpr uint32_t XSM_POOL_SIZE = 2048;

static constexpr uint32_t XSM_UL_QUEUE_INIT_SIZE = 128;

static constexpr uint32_t XSM_DL_MAX_PER_SLOT = 256;

static constexpr uint32_t XSM_DL_SLOT_DEPTH = 16;

static constexpr uint32_t XSM_UL_MAX_PER_SLOT = 256;

static constexpr uint32_t XSM_UL_SLOT_DEPTH = 16;

static constexpr uint16_t XSM_FLAG_SG_FIRST = (1 << 15) | (1 << 9);
static constexpr uint16_t XSM_FLAG_SG_NEXT  = (1 << 15);
static constexpr uint16_t XSM_FLAG_SG_LAST  = (1 << 15) | (1 << 8);

struct xsm_buffer_desc {
  void*             va = nullptr;
  uint64_t          pa = 0;
  std::atomic<bool> in_use{false};
  xsm_buffer_desc*  next = nullptr;
};

struct xsm_dl_slot {
  xsm_buffer_desc* buffers[XSM_DL_MAX_PER_SLOT] = {};
  uint32_t         count                        = 0;
};

struct xsm_ul_slot {
  xsm_buffer_desc* buffers[XSM_UL_MAX_PER_SLOT] = {};
  uint32_t         count                        = 0;
};

struct xsm_context_stats {
  std::atomic<uint64_t> total_alloc{0};
  std::atomic<uint64_t> total_free{0};
  std::atomic<uint64_t> alloc_fail{0};
  std::atomic<uint64_t> xsm_put_cnt{0};
  std::atomic<uint64_t> xsm_get_cnt{0};
  std::atomic<uint64_t> xsm_wait_cnt{0};
  std::atomic<uint64_t> ul_blocks_enqueued{0};
  std::atomic<uint32_t> current_alloc{0};
  std::atomic<uint32_t> current_free{0};
};

class xsm_context
{
public:
  xsm_context() = default;
  ~xsm_context();

  xsm_context(const xsm_context&)            = delete;
  xsm_context& operator=(const xsm_context&) = delete;

  bool dpdk_init(const std::string& file_prefix, bool is_primary);

  bool open(const std::string& device_name, bool is_master,
            uint64_t mac_mem_size, uint64_t phy_mem_size,
            uint32_t pair_index = 0, uint32_t num_pairs = 1);

  bool wait_for_peer(std::chrono::milliseconds timeout);

  void close();

  bool is_open()       const { return initialized_; }
  bool is_master()     const { return is_master_; }
  bool is_peer_alive() const { return peer_alive_.load(); }

  void* alloc_buffer();

  void free_buffer(void* ptr);

  xsm_buffer_desc* find_buffer(void* ptr);

  xsm_buffer_desc* find_buffer_by_pa(uint64_t pa);

  uint32_t num_free_buffers() const { return stats_.current_free.load(); }

  uint64_t va_to_pa(void* va);

  void* pa_to_va(uint64_t pa);

  int put(void* va_msg, uint32_t msg_size, uint16_t msg_type, uint16_t flags);

  int wait();

  int check();

  void* get(uint32_t& msg_size, uint16_t& msg_type, uint16_t& flags);

  void* get(uint32_t& msg_size, uint16_t& msg_type, uint16_t& flags, uint64_t& payload_pa);

  void return_pa(uint64_t payload_pa);

  void wake_up();

  uint32_t enqueue_ul_blocks();

  void* dequeue_ul_block();

  void add_dl_buffer_to_current_slot(xsm_buffer_desc* buf);

  void rotate_dl_slots();

  void add_ul_buffer_to_current_slot(xsm_buffer_desc* buf);

  void rotate_ul_slots();

  const xsm_context_stats& get_stats() const { return stats_; }
  void                     print_stats() const;

private:
  bool init_pool();
  void init_dl_slots();

  void*             xsm_handle_     = nullptr;
  void*             shared_memory_  = nullptr;
  uint64_t          total_mem_size_ = 0;
  bool              is_master_      = false;
  bool              initialized_    = false;
  std::atomic<bool> peer_alive_{false};

  xsm_buffer_desc  pool_buffers_[XSM_POOL_SIZE];
  xsm_buffer_desc* free_head_    = nullptr;
  uint8_t*         pool_base_va_ = nullptr;
  uint64_t         pa_base_      = 0;
  uint64_t         pa_end_       = 0;
  std::mutex       pool_mutex_;

  xsm_dl_slot dl_slots_[XSM_DL_SLOT_DEPTH];
  std::mutex  dl_mutex_;

  xsm_ul_slot ul_slots_[XSM_UL_SLOT_DEPTH];
  std::mutex  ul_mutex_;

  xsm_context_stats stats_;
};

}
