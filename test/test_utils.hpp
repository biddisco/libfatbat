#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>
//
#include "libfatbat/logging.hpp"
//
#include "test/communicator.hpp"
#include "test/controller.hpp"
#include "test/operation_context.hpp"
#include "test/polling_helper.hpp"

// ------------------------------------------------------------------
MAKE_LOGGER(testutil_log, "TestUtil")

// RMA key info structure for exchanging registration details
struct rma_key_info
{
  void* address;
  uint64_t remote_key;
  uint64_t length;
};

// ------------------------------------------------------------------
// Wait for all send/recv completions
inline void wait_for_msg_completions(test_controller& controller)
{
  while ((uint32_t) controller.sends_complete_ < controller.sends_posted_ ||
      (uint32_t) controller.recvs_complete_ < controller.recvs_posted_)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

// ------------------------------------------------------------------
// Wait for write completions up to a target count
inline void wait_for_write_completions(test_controller& controller, uint32_t target)
{
  while ((uint32_t) controller.writes_complete_ < target)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

// ------------------------------------------------------------------
// Wait until an atomic counter reaches at least the requested target.
inline void wait_for_counter(std::atomic<uint32_t> const& counter, uint32_t target)
{
  while (counter.load(std::memory_order_acquire) < target)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

// ------------------------------------------------------------------
// Semaphore structure for remote completion notification
// creating a semaphore involves key exchange of buffer info so it is a collective operation
template <typename HeapType>
struct semaphore_info
{
  typename HeapType::pointer
      local_buffer;               // Registered memory written into by peers (1 slot per rank)
  rma_key_info local_key_info;    // Key info for our local memory buffer
  std::vector<rma_key_info> remote_key_infos;    // Key info for remote buffers: 1 per rank
  HeapType& heap_;
  communicator* comm_;

  // setup buffers and exchange keys with peers in the constructor, using the provided communicator and heap
  semaphore_info(communicator* comm, HeapType& heap, size_t rank, size_t size)
    : heap_(heap)
    , comm_(comm)
  {
    // allocate a local buffer for signals to be written into
    local_buffer = heap.allocate(sizeof(std::uint64_t) * size, 0);
    // get the key info for our local buffer
    local_key_info = rma_key_info{
        .address = local_buffer.handle().get_address(),
        .remote_key = (uint64_t) local_buffer.handle().get_remote_key(),
        .length = sizeof(std::uint64_t) * size,
    };
    // create space for key info for each peer's signalling buffer
    remote_key_infos.resize(size);

    // this needs to run inside polling scope to ensure progress
    {
      // poller_guard pg(comm.m_controller, rank, 1);
      // Exchange semaphore keys with all peers
      for (size_t r = 0; r < size; ++r)
      {
        if (r == rank)
        {
          remote_key_infos[r] = local_key_info;    // Fill in our own key info for convenience
          continue;
        }
        // each rank will send us the key for their buffer for us to write signal completions into
        typename HeapType::pointer recv_buffer = heap.allocate(sizeof(rma_key_info), 0);
        typename HeapType::pointer send_buffer = heap.allocate(sizeof(rma_key_info), 0);

        // receive key info from each rank
        comm_->recv(recv_buffer, sizeof(rma_key_info), static_cast<rank_type>(r),
            static_cast<tag_type>(2000 + r), [recv_buffer, r, this, &heap](rank_type, tag_type) {
              remote_key_infos[r] = *static_cast<rma_key_info*>(recv_buffer.get());
              heap.free(recv_buffer);
            });

        // send our key info to each rank
        std::memcpy(send_buffer.get(), &local_key_info, sizeof(rma_key_info));
        comm_->send(send_buffer, sizeof(rma_key_info), static_cast<rank_type>(r),
            static_cast<tag_type>(2000 + rank),
            [send_buffer, &heap](rank_type, tag_type) { heap.free(send_buffer); });
      }
      wait_for_msg_completions(*comm_->m_controller);
    }
  }
  //
  ~semaphore_info() { heap_.free(local_buffer); }
  //
  void signal_completion(std::uint64_t signal, size_t rank)
  {
    // write a 'signal value' into the remote rank's slot in their local buffer to signal completion
    auto& remote_info = remote_key_infos[rank];
    uint64_t const slot_offset = sizeof(std::uint64_t) * comm_->rank();
    uint64_t const remote_addr = comm_->m_controller->use_relative_remote_addr() ?
        slot_offset :
        (uint64_t(remote_info.address) + slot_offset);
    comm_->inject_write_fenced(
        signal, static_cast<rank_type>(rank), remote_addr, remote_info.remote_key, nullptr);
  }
  //
  std::uint64_t read_completion(size_t rank)
  {
    // read the signal value from our local buffer for the specified rank
    auto* slot = static_cast<std::uint64_t*>(local_buffer.get()) + rank;
    return *slot;
  }
};

// ------------------------------------------------------------------
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
inline auto verify_buffer = [](void const* buffer, std::size_t message_size, rank_type this_rank,
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