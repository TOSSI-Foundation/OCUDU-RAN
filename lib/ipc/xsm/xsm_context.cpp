#include "xsm_context.h"

#include "ocudu/xsm/xsm/xsm.h"

#include <rte_eal.h>
#include <rte_pdump.h>

#include <cstdio>
#include <cstring>
#include <thread>

using namespace ocudu;

xsm_context::~xsm_context()
{
  if (initialized_) {
    close();
  }
}

static void cleanup_stale_dpdk_files(const std::string& file_prefix)
{
  std::string dpdk_dir  = "/var/run/dpdk/" + file_prefix;
  std::string lock_file = dpdk_dir + "/config";
  FILE*       f         = std::fopen(lock_file.c_str(), "r");
  if (f != nullptr) {
    std::fclose(f);
    std::string cmd = "rm -rf " + dpdk_dir;
    int         rc  = std::system(cmd.c_str());
    (void)rc;
  }
}

bool xsm_context::dpdk_init(const std::string& file_prefix, bool is_primary)
{
  const char* proc_type = is_primary ? "primary" : "auto";

  if (is_primary) {
    cleanup_stale_dpdk_files(file_prefix);
  }

  char lcores_arg[64];
  char proc_type_arg[32];
  char file_prefix_arg[128];
  char iova_mode_arg[32];

  std::snprintf(lcores_arg, sizeof(lcores_arg), "(0-1)@(0,2,4)");
  std::snprintf(proc_type_arg, sizeof(proc_type_arg), "--proc-type=%s", proc_type);
  std::snprintf(file_prefix_arg, sizeof(file_prefix_arg), "%s", file_prefix.c_str());
  std::snprintf(iova_mode_arg, sizeof(iova_mode_arg), "--iova-mode=pa");

  char* argv[] = {
      const_cast<char*>("ocudu"),
      const_cast<char*>("--lcores"),
      lcores_arg,
      proc_type_arg,
      const_cast<char*>("--file-prefix"),
      file_prefix_arg,
      iova_mode_arg,
  };
  int argc = sizeof(argv) / sizeof(argv[0]);

  int rc = rte_eal_init(argc, argv);
  if (rc < 0) {
    std::fprintf(stderr, "xsm_context: rte_eal_init failed (%d)\n", rc);
    return false;
  }

  rte_pdump_init();
  return true;
}

bool xsm_context::open(const std::string& device_name, bool is_master,
                       uint64_t mac_mem_size, uint64_t phy_mem_size,
                       uint32_t pair_index, uint32_t num_pairs)
{
  if (initialized_) {
    return false;
  }

  is_master_ = is_master;

  uint64_t total = mac_mem_size + phy_mem_size;

  xsm_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  std::snprintf(cfg.device_name, XSM_DEVICE_NAME_MAX, "%s", device_name.c_str());
  cfg.role        = is_master ? XSM_ROLE_MASTER : XSM_ROLE_SLAVE;
  cfg.memory_size = is_master ? 0u : total;
  cfg.pair_index  = pair_index;
  cfg.num_pairs   = num_pairs;

  xsm_handle_t* handle = nullptr;
  xsm_status_t  st     = XSM_Open(&cfg, &handle);
  if (st != XSM_OK) {
    std::fprintf(stderr, "xsm_context: XSM_Open: %s\n", xsm_strerror(st));
    return false;
  }
  xsm_handle_ = handle;

  void* region = nullptr;
  st           = XSM_Alloc(handle, total, &region);
  if (st != XSM_OK) {
    std::fprintf(stderr, "xsm_context: XSM_Alloc: %s\n", xsm_strerror(st));
    XSM_Close(handle);
    xsm_handle_ = nullptr;
    return false;
  }
  shared_memory_  = region;
  total_mem_size_ = total;

  if (!init_pool()) {
    XSM_Free(handle, region);
    XSM_Close(handle);
    xsm_handle_    = nullptr;
    shared_memory_ = nullptr;
    return false;
  }

  init_dl_slots();
  for (uint32_t i = 0; i < XSM_UL_SLOT_DEPTH; ++i) {
    ul_slots_[i].count = 0;
    std::memset(ul_slots_[i].buffers, 0, sizeof(ul_slots_[i].buffers));
  }

  initialized_ = true;
  return true;
}

bool xsm_context::wait_for_peer(std::chrono::milliseconds timeout)
{
  if (!initialized_ || xsm_handle_ == nullptr) {
    return false;
  }

  auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (XSM_IsPeerReady(static_cast<xsm_handle_t*>(xsm_handle_)) == XSM_OK) {
      peer_alive_.store(true);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

void xsm_context::close()
{
  if (!initialized_) {
    return;
  }

  peer_alive_.store(false);

  {
    std::lock_guard<std::mutex> lock(dl_mutex_);
    for (int slot = 0; slot < static_cast<int>(XSM_DL_SLOT_DEPTH); ++slot) {
      for (uint32_t i = 0; i < dl_slots_[slot].count; ++i) {
        xsm_buffer_desc* buf = dl_slots_[slot].buffers[i];
        if (buf != nullptr) {
          free_buffer(buf->va);
        }
      }
      dl_slots_[slot].count = 0;
    }
  }

  {
    std::lock_guard<std::mutex> lock(ul_mutex_);
    for (int slot = 0; slot < static_cast<int>(XSM_UL_SLOT_DEPTH); ++slot) {
      for (uint32_t i = 0; i < ul_slots_[slot].count; ++i) {
        xsm_buffer_desc* buf = ul_slots_[slot].buffers[i];
        if (buf != nullptr) {
          free_buffer(buf->va);
        }
      }
      ul_slots_[slot].count = 0;
    }
  }

  print_stats();

  auto* h = static_cast<xsm_handle_t*>(xsm_handle_);
  if (is_master_ && shared_memory_ != nullptr && h != nullptr) {
    XSM_Free(h, shared_memory_);
  }
  if (h != nullptr) {
    XSM_Close(h);
  }

  xsm_handle_    = nullptr;
  shared_memory_ = nullptr;
  initialized_   = false;
}

bool xsm_context::init_pool()
{
  uint64_t required = static_cast<uint64_t>(XSM_BLOCK_SIZE) * XSM_POOL_SIZE;
  if (required * 2 > total_mem_size_) {
    return false;
  }

  free_head_ = nullptr;
  stats_.current_free.store(XSM_POOL_SIZE);
  stats_.current_alloc.store(0);

  const uint64_t pool_offset = is_master_ ? 0 : required;
  pool_base_va_              = static_cast<uint8_t*>(shared_memory_) + pool_offset;
  auto* mem_ptr              = pool_base_va_;

  auto* h = static_cast<xsm_handle_t*>(xsm_handle_);
  for (uint32_t i = 0; i < XSM_POOL_SIZE; ++i) {
    xsm_buffer_desc& buf = pool_buffers_[i];
    buf.va               = mem_ptr;
    buf.in_use.store(false);

    buf.pa = XSM_VirtToPhys(h, buf.va);
    if (buf.pa == 0) {
      return false;
    }
    if (i == 0) {
      pa_base_ = buf.pa;
    }

    buf.next   = free_head_;
    free_head_ = &buf;

    std::memset(buf.va, 0, XSM_BLOCK_SIZE);
    mem_ptr += XSM_BLOCK_SIZE;
  }

  pa_end_ = pa_base_ + required;
  return true;
}

void* xsm_context::alloc_buffer()
{
  std::lock_guard<std::mutex> lock(pool_mutex_);

  if (free_head_ == nullptr) {
    stats_.alloc_fail.fetch_add(1);
    return nullptr;
  }

  xsm_buffer_desc* buf = free_head_;
  free_head_           = buf->next;
  buf->next            = nullptr;
  buf->in_use.store(true);

  stats_.total_alloc.fetch_add(1);
  stats_.current_alloc.fetch_add(1);
  stats_.current_free.fetch_sub(1);

  return buf->va;
}

void xsm_context::free_buffer(void* ptr)
{
  if (ptr == nullptr) {
    return;
  }

  xsm_buffer_desc* buf = find_buffer(ptr);
  if (buf == nullptr) {
    return;
  }
  if (!buf->in_use.load()) {
    return;
  }

  std::lock_guard<std::mutex> lock(pool_mutex_);
  buf->in_use.store(false);
  buf->next  = free_head_;
  free_head_ = buf;

  stats_.total_free.fetch_add(1);
  stats_.current_alloc.fetch_sub(1);
  stats_.current_free.fetch_add(1);
}

xsm_buffer_desc* xsm_context::find_buffer(void* ptr)
{
  if (ptr == nullptr || pool_base_va_ == nullptr) {
    return nullptr;
  }

  auto* base = pool_base_va_;
  auto* end  = base + static_cast<uint64_t>(XSM_BLOCK_SIZE) * XSM_POOL_SIZE;
  if (static_cast<uint8_t*>(ptr) < base || static_cast<uint8_t*>(ptr) >= end) {
    return nullptr;
  }

  uint64_t offset = static_cast<uint8_t*>(ptr) - base;
  uint32_t idx    = static_cast<uint32_t>(offset / XSM_BLOCK_SIZE);
  if (idx >= XSM_POOL_SIZE || pool_buffers_[idx].va != ptr) {
    return nullptr;
  }
  return &pool_buffers_[idx];
}

xsm_buffer_desc* xsm_context::find_buffer_by_pa(uint64_t pa)
{
  if (pa == 0 || pa < pa_base_ || pa >= pa_end_) {
    return nullptr;
  }
  uint64_t offset = pa - pa_base_;
  uint32_t idx    = static_cast<uint32_t>(offset / XSM_BLOCK_SIZE);
  if (idx >= XSM_POOL_SIZE || pool_buffers_[idx].pa != pa) {
    return nullptr;
  }
  return &pool_buffers_[idx];
}

uint64_t xsm_context::va_to_pa(void* va)
{
  if (xsm_handle_ == nullptr || va == nullptr) {
    return 0;
  }
  xsm_buffer_desc* buf = find_buffer(va);
  if (buf != nullptr) {
    return buf->pa;
  }
  return XSM_VirtToPhys(static_cast<xsm_handle_t*>(xsm_handle_), va);
}

void* xsm_context::pa_to_va(uint64_t pa)
{
  if (xsm_handle_ == nullptr || pa == 0) {
    return nullptr;
  }
  xsm_buffer_desc* buf = find_buffer_by_pa(pa);
  if (buf != nullptr) {
    return buf->va;
  }
  return XSM_PhysToVirt(static_cast<xsm_handle_t*>(xsm_handle_), pa);
}

int xsm_context::put(void* va_msg, uint32_t msg_size, uint16_t msg_type, uint16_t flags)
{
  if (xsm_handle_ == nullptr) {
    return -1;
  }
  if (!peer_alive_.load()) {
    return -1;
  }

  uint64_t pa = va_to_pa(va_msg);
  if (pa == 0) {
    return -1;
  }

  xsm_msg_t msg = { pa, msg_size, msg_type, flags };
  xsm_status_t st = XSM_Put(static_cast<xsm_handle_t*>(xsm_handle_), &msg);
  stats_.xsm_put_cnt.fetch_add(1);

  if (st != XSM_OK) {
    peer_alive_.exchange(false);
    return -1;
  }
  return 0;
}

int xsm_context::wait()
{
  if (xsm_handle_ == nullptr) {
    return -1;
  }
  auto* h = static_cast<xsm_handle_t*>(xsm_handle_);

  xsm_status_t st = XSM_Wait(h, UINT32_MAX);
  stats_.xsm_wait_cnt.fetch_add(1);
  if (st != XSM_OK) {
    return -1;
  }
  return static_cast<int>(XSM_Pending(h));
}

int xsm_context::check()
{
  if (xsm_handle_ == nullptr) {
    return 0;
  }
  return static_cast<int>(XSM_Pending(static_cast<xsm_handle_t*>(xsm_handle_)));
}

void* xsm_context::get(uint32_t& msg_size, uint16_t& msg_type, uint16_t& flags)
{
  uint64_t pa = 0;
  return get(msg_size, msg_type, flags, pa);
}

void* xsm_context::get(uint32_t& msg_size, uint16_t& msg_type, uint16_t& flags, uint64_t& payload_pa)
{
  payload_pa = 0;
  if (xsm_handle_ == nullptr) {
    return nullptr;
  }
  xsm_msg_t msg;
  xsm_status_t st = XSM_Get(static_cast<xsm_handle_t*>(xsm_handle_), &msg);
  if (st != XSM_OK) {
    return nullptr;
  }

  msg_size   = msg.payload_size;
  msg_type   = msg.type_id;
  flags      = msg.flags;
  payload_pa = msg.payload_pa;
  stats_.xsm_get_cnt.fetch_add(1);
  return pa_to_va(msg.payload_pa);
}

void xsm_context::return_pa(uint64_t payload_pa)
{
  if (xsm_handle_ == nullptr || payload_pa == 0) {
    return;
  }

  XSM_ReturnBuffer(static_cast<xsm_handle_t*>(xsm_handle_), payload_pa);
}

void xsm_context::wake_up()
{
  if (xsm_handle_ != nullptr) {
    XSM_Notify(static_cast<xsm_handle_t*>(xsm_handle_));
  }
}

uint32_t xsm_context::enqueue_ul_blocks()
{
  uint32_t n = 0;
  while (n < XSM_UL_QUEUE_INIT_SIZE) {
    void* ptr = alloc_buffer();
    if (ptr == nullptr) {
      break;
    }
    xsm_buffer_desc* buf = find_buffer(ptr);
    if (buf == nullptr) {
      free_buffer(ptr);
      break;
    }

    xsm_status_t st =
        XSM_ReturnBuffer(static_cast<xsm_handle_t*>(xsm_handle_), buf->pa);
    if (st != XSM_OK) {
      free_buffer(ptr);
      break;
    }
    ++n;
    stats_.ul_blocks_enqueued.fetch_add(1);
  }
  return n;
}

void* xsm_context::dequeue_ul_block()
{
  if (xsm_handle_ == nullptr) {
    return nullptr;
  }
  uint64_t pa = 0;
  xsm_status_t st =
      XSM_AcquireBuffer(static_cast<xsm_handle_t*>(xsm_handle_), &pa);
  if (st != XSM_OK || pa == 0) {
    return nullptr;
  }
  return pa_to_va(pa);
}

void xsm_context::init_dl_slots()
{
  for (uint32_t i = 0; i < XSM_DL_SLOT_DEPTH; ++i) {
    dl_slots_[i].count = 0;
    std::memset(dl_slots_[i].buffers, 0, sizeof(dl_slots_[i].buffers));
  }
}

void xsm_context::add_dl_buffer_to_current_slot(xsm_buffer_desc* buf)
{
  if (buf == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(dl_mutex_);
  xsm_dl_slot&                slot = dl_slots_[XSM_DL_SLOT_DEPTH - 1];
  if (slot.count >= XSM_DL_MAX_PER_SLOT) {
    return;
  }
  slot.buffers[slot.count] = buf;
  slot.count++;
}

void xsm_context::rotate_dl_slots()
{
  std::lock_guard<std::mutex> lock(dl_mutex_);

  for (uint32_t i = 0; i < dl_slots_[0].count; ++i) {
    xsm_buffer_desc* buf = dl_slots_[0].buffers[i];
    if (buf != nullptr) {
      free_buffer(buf->va);
    }
  }

  for (uint32_t k = 0; k + 1 < XSM_DL_SLOT_DEPTH; ++k) {
    xsm_dl_slot& dst = dl_slots_[k];
    xsm_dl_slot& src = dl_slots_[k + 1];
    for (uint32_t i = 0; i < src.count; ++i) {
      dst.buffers[i] = src.buffers[i];
    }
    dst.count = src.count;
  }
  dl_slots_[XSM_DL_SLOT_DEPTH - 1].count = 0;
}

void xsm_context::add_ul_buffer_to_current_slot(xsm_buffer_desc* buf)
{
  if (buf == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(ul_mutex_);
  xsm_ul_slot&                slot = ul_slots_[XSM_UL_SLOT_DEPTH - 1];
  if (slot.count >= XSM_UL_MAX_PER_SLOT) {
    free_buffer(buf->va);
    return;
  }
  slot.buffers[slot.count] = buf;
  slot.count++;
}

void xsm_context::rotate_ul_slots()
{
  std::lock_guard<std::mutex> lock(ul_mutex_);

  for (uint32_t i = 0; i < ul_slots_[0].count; ++i) {
    xsm_buffer_desc* buf = ul_slots_[0].buffers[i];
    if (buf != nullptr) {
      free_buffer(buf->va);
    }
  }

  for (uint32_t k = 0; k + 1 < XSM_UL_SLOT_DEPTH; ++k) {
    xsm_ul_slot& dst = ul_slots_[k];
    xsm_ul_slot& src = ul_slots_[k + 1];
    for (uint32_t i = 0; i < src.count; ++i) {
      dst.buffers[i] = src.buffers[i];
    }
    dst.count = src.count;
  }
  ul_slots_[XSM_UL_SLOT_DEPTH - 1].count = 0;
}

void xsm_context::print_stats() const
{

}
