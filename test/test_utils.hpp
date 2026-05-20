#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>
//
#include "libfatbat/logging.hpp"
//
#include "test/controller.hpp"
#include "test/operation_context.hpp"

// ------------------------------------------------------------------
MAKE_LOGGER(testutil_log, "TestUtil")

// RMA key info structure for exchanging registration details
struct rma_key_info
{
  void* address;
  uint64_t remote_key;
  uint64_t length;
};

// Semaphore structure for remote completion notification
template <typename HeapType>
struct semaphore_info
{
  typename HeapType::pointer local_buffer;       // Registered local buffer for signaling
  typename HeapType::pointer local_key_info;     // Key info for our semaphore buffer
  typename HeapType::pointer remote_key_info;    // Remote peer's semaphore key info
};

// Wait for all send/recv completions
inline void wait_for_msg_completions(test_controller& controller)
{
  while ((uint32_t) controller.sends_complete_ < controller.sends_posted_ ||
      (uint32_t) controller.recvs_complete_ < controller.recvs_posted_)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

// Wait for write completions up to a target count
inline void wait_for_write_completions(test_controller& controller, uint32_t target)
{
  while ((uint32_t) controller.writes_complete_ < target)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

// Wait until an atomic counter reaches at least the requested target.
inline void wait_for_counter(std::atomic<uint32_t> const& counter, uint32_t target)
{
  while (counter.load(std::memory_order_acquire) < target)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

// Installs a remote CQ-data callback and guarantees it is cleared before captured state dies.
struct remote_cq_data_callback_scope
{
  test_controller& controller_;

  template <typename Callback>
  remote_cq_data_callback_scope(test_controller& controller, Callback&& callback)
    : controller_(controller)
  {
    controller_.remote_cq_data_callback_ = std::forward<Callback>(callback);
  }

  remote_cq_data_callback_scope(remote_cq_data_callback_scope const&) = delete;
  remote_cq_data_callback_scope& operator=(remote_cq_data_callback_scope const&) = delete;

  ~remote_cq_data_callback_scope() { controller_.remote_cq_data_callback_ = nullptr; }
};

// create a lambda we can use as a callback function that verifies the data in the buffer is correct
auto verify_buffer = [](void const* buffer, std::size_t message_size, rank_type this_rank,
                         std::uint8_t expected, char const* msg, rank_type remote_rank,
                         tag_type /*tag*/) {
  // verify the RMA/MSG buffer content: every byte must match the expected value
  auto* data = static_cast<uint8_t const*>(buffer);
  for (std::size_t i = 0; i < static_cast<std::size_t>(message_size); ++i)
  {
    if (data[i] != static_cast<uint8_t>(expected))
    {
      LIBFATBAT_TRACE(
          testutil_log, "{:<20} {}", msg, libfatbat::log::mem_crc32(buffer, message_size));
      LIBFATBAT_ERROR(testutil_log,
          "{:<20} rank {} Buffer validation failed: src {} index {}/{} value {} expected {}", msg,
          this_rank, remote_rank, i, message_size, data[i], static_cast<uint8_t>(expected));
      throw std::runtime_error("Buffer validation failed");
    }
  }
  LIBFATBAT_TRACE(testutil_log, "{:<20} buffer validation successful", msg);
};