/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
//
#include <boost/program_options.hpp>
#include <hwmalloc/heap.hpp>
//
#include "libfatbat/logging.hpp"
//
#include "test/communicator.hpp"
#include "test/controller.hpp"
#include "test/pmi_helper.hpp"
#include "test/polling_helper.hpp"
#include "test/test_utils.hpp"

// ------------------------------------------------------------------
MAKE_LOGGER(rdmawritetest_log, "RdmaWriteTest")

namespace {

  struct rma_key_info
  {
    void* address;
    uint64_t remote_key;
    uint64_t length;
  };

  void wait_for_msg_completions(test_controller& controller)
  {
    while ((uint32_t) controller.sends_complete_ < controller.sends_posted_ ||
        (uint32_t) controller.recvs_complete_ < controller.recvs_posted_)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
  }

  void wait_for_write_completions(test_controller& controller, uint32_t target)
  {
    while ((uint32_t) controller.writes_complete_ < target)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
  }

}    // namespace

// ----------------------------------------------------------------------------

int main(int argc, char** argv)
{
  libfatbat::log::init_from_env();

  namespace po = boost::program_options;
  po::options_description desc("Options");
  desc.add_options()("debug", "Enable debug mode");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  bool attach_debugger = vm.count("debug") > 0;

  std::size_t rank, size;
  std::size_t nthreads = 2;
  test_controller controller;
  pmi_helper pmi;

  std::tie(rank, size) = pmi.init_PMI(attach_debugger);
  controller.initialize(LIBFATBAT_HAVE_PROVIDER, rank, size, nthreads);
  pmi.boot_PMI(&controller);
  pmi.fence();

  if (size < 2)
  {
    LIBFATBAT_ERROR(rdmawritetest_log, "This test requires at least 2 ranks.");
    return EXIT_FAILURE;
  }

  memory_context c(&controller);
  memory_context::heap_type heap(&c);
  communicator comm(&controller, rank, size);

  // dedicated background thread for polling completions
  poller_guard pg(&controller, rank);

  constexpr int32_t message_size = 1024 * 1024 * 16;
  constexpr std::size_t footer_size = sizeof(int32_t);
  constexpr std::size_t target_buffer_size = message_size + footer_size;
  constexpr std::size_t iterations = 5;

  std::vector<memory_context::heap_type::pointer> rma_write_keys;
  std::vector<memory_context::heap_type::pointer> rma_target_buffers;
  std::vector<memory_context::heap_type::pointer> local_source_keys;
  std::vector<memory_context::heap_type::pointer> local_source_buffers;

  // Semaphore structure for remote completion notification.
  struct semaphore_info
  {
    memory_context::heap_type::pointer local_buffer;       // Registered local buffer for writing
    memory_context::heap_type::pointer local_key_info;     // Key info for our semaphore buffer
    memory_context::heap_type::pointer remote_key_info;    // Remote key/address info
  };
  std::vector<semaphore_info> semaphores(size);

  for (int i = 0; i < static_cast<int>(size); i++)
  {
    auto target_buffer = heap.allocate(target_buffer_size, 0);
    std::fill((uint8_t*) target_buffer.get(), (uint8_t*) target_buffer.get() + target_buffer_size,
        uint8_t(0xEE));
    rma_target_buffers.push_back(target_buffer);

    auto remote_key_buffer = heap.allocate(sizeof(rma_key_info), 0);
    rma_write_keys.push_back(remote_key_buffer);

    auto source_buffer = heap.allocate(message_size, 0);
    std::fill((uint8_t*) source_buffer.get(), (uint8_t*) source_buffer.get() + message_size,
        static_cast<uint8_t>(
            rank + 17));    // Use a different pattern than the target buffer to help debugging
    local_source_buffers.push_back(source_buffer);

    auto source_key = heap.allocate(sizeof(rma_key_info), 0);
    rma_key_info info{
        .address = target_buffer.handle().get_address(),
        .remote_key = (uint64_t) target_buffer.handle().get_remote_key(),
        .length = target_buffer_size,
    };
    std::memcpy(source_key.get(), &info, sizeof(rma_key_info));
    local_source_keys.push_back(source_key);

    // Semaphore: allocate local buffer, key info buffer for local buffer, and space for remote key info
    semaphores[i].local_buffer = heap.allocate(sizeof(int32_t), 0);
    semaphores[i].local_key_info = heap.allocate(sizeof(rma_key_info), 0);
    semaphores[i].remote_key_info = heap.allocate(sizeof(rma_key_info), 0);

    // Fill key info for our semaphore buffer
    rma_key_info sem_info{
        .address = semaphores[i].local_buffer.handle().get_address(),
        .remote_key = (uint64_t) semaphores[i].local_buffer.handle().get_remote_key(),
        .length = sizeof(int32_t),
    };
    std::memcpy(semaphores[i].local_key_info.get(), &sem_info, sizeof(rma_key_info));
  }

  LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {}", "initialized", rank);

  for (int r = 0; r < static_cast<int>(size); ++r)
  {
    if (rank == static_cast<size_t>(r)) continue;

    comm.recv(rma_write_keys[r], sizeof(rma_key_info), r, r, nullptr);
    comm.send(local_source_keys[r], sizeof(rma_key_info), r, rank, nullptr);

    // Semaphore: exchange key info for semaphore buffers
    comm.recv(semaphores[r].remote_key_info, sizeof(rma_key_info), r, 1000 + r, nullptr);
    comm.send(semaphores[r].local_key_info, sizeof(rma_key_info), r, 1000 + rank, nullptr);
  }

  wait_for_msg_completions(controller);
  LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {}", "key exchange complete", rank);
  pmi.fence();

  for (std::size_t it = 0; it < iterations; ++it)
  {
    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      std::fill((uint8_t*) rma_target_buffers[r].get(),
          (uint8_t*) rma_target_buffers[r].get() + target_buffer_size, uint8_t(0xEE));
    }

    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;

      auto* remote_key_info = static_cast<rma_key_info*>(rma_write_keys[r].get());
      if (remote_key_info->length < static_cast<uint64_t>(target_buffer_size))
      {
        LIBFATBAT_ERROR(rdmawritetest_log, "rank {} got invalid RMA key length {} from rank {}",
            rank, remote_key_info->length, r);
        return EXIT_FAILURE;
      }

      // With FI_MR_VIRT_ADDR disabled, use offset 0 for provider-relative addressing.
      uint64_t remote_addr = 0;
      comm.write(local_source_buffers[r], message_size, r, remote_addr, remote_key_info->remote_key,
          nullptr);
      LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} -> rank {} key {:#08x}", "fi_write posted",
          rank, r, remote_key_info->remote_key);
    }

    uint32_t const data_write_target = static_cast<uint32_t>(controller.writes_posted_);
    wait_for_write_completions(controller, data_write_target);
    LIBFATBAT_INFO(
        rdmawritetest_log, "{:<20} rank {} iteration {}", "data writes complete", rank, it);

    // Post a final footer write inside the peer's existing target buffer.
    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      // Use the semaphore's own remote key info and buffer
      auto* sem_remote_key_info = static_cast<rma_key_info*>(semaphores[r].remote_key_info.get());
      uint64_t sem_remote_addr = 0;    // Always offset 0 in the semaphore buffer
      *(int32_t*) semaphores[r].local_buffer.get() = 1;
      comm.write(semaphores[r].local_buffer, sizeof(int32_t), r, sem_remote_addr,
          sem_remote_key_info->remote_key, nullptr);
      LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} -> rank {} semaphore write",
          "semaphore write", rank, r);
    }

    // Wait for the semaphore writes to complete locally after they have been accepted.
    uint32_t const write_target = static_cast<uint32_t>(controller.writes_posted_);
    wait_for_write_completions(controller, write_target);
    LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} iteration {}", "writes complete", rank, it);

    // Wait for all remote peers to set our local semaphore buffer.
    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      auto* sem_local_ptr = reinterpret_cast<int32_t volatile*>(semaphores[r].local_buffer.get());
      while (*sem_local_ptr != 1)
      {
        comm.progress();
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
      *sem_local_ptr = 0;
      LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} saw remote semaphore from {}",
          "semaphore seen", rank, r);
    }

    // Collective barrier: ensures all ranks have at least TX-completed their writes.
    // On SHM (and similar providers), TX completion == delivery, so data is visible here.
    pmi.fence();

    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      // Validate that our buffer contains the sender's value (r)
      verify_buffer(rma_target_buffers[r].get(), message_size, rank, static_cast<uint8_t>(r + 17),
          "write validation", r, 0);
    }

    LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} iteration {}", "verify complete", rank, it);
    pmi.fence();
  }

  pmi.finalize_PMI();

  for (auto& buf : rma_write_keys) { heap.free(buf); }
  for (auto& buf : rma_target_buffers) { heap.free(buf); }
  for (auto& buf : local_source_keys) { heap.free(buf); }
  for (auto& buf : local_source_buffers) { heap.free(buf); }
  for (auto& sem : semaphores)
  {
    heap.free(sem.local_buffer);
    heap.free(sem.local_key_info);
    heap.free(sem.remote_key_info);
  }

  return 0;
}
