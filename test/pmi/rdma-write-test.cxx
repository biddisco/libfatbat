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

// Using shared utilities from test_utils.hpp:
// - rma_key_info struct
// - wait_for_msg_completions()
// - wait_for_write_completions()
// - semaphore_info<> struct

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
  constexpr std::size_t iterations = 5;

  std::vector<memory_context::heap_type::pointer> rma_write_keys;
  std::vector<memory_context::heap_type::pointer> rma_target_buffers;
  std::vector<memory_context::heap_type::pointer> local_source_keys;
  std::vector<memory_context::heap_type::pointer> local_source_buffers;

  for (int i = 0; i < static_cast<int>(size); i++)
  {
    auto target_buffer = heap.allocate(message_size, 0);
    std::fill((uint8_t*) target_buffer.get(), (uint8_t*) target_buffer.get() + message_size,
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
        .length = message_size,
    };
    std::memcpy(source_key.get(), &info, sizeof(rma_key_info));
    local_source_keys.push_back(source_key);
  }

  LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {}", "initialized", rank);

  exchange_rma_keys(comm, controller, local_source_keys, rma_write_keys);
  LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {}", "key exchange complete", rank);
  pmi.fence();

  for (std::size_t it = 0; it < iterations; ++it)
  {
    // Clear target buffers for this iteration
    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      std::fill((uint8_t*) rma_target_buffers[r].get(),
          (uint8_t*) rma_target_buffers[r].get() + message_size, uint8_t(0xEE));
    }

    // Post data writes to all peers (not self)
    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      auto* remote_key_info = static_cast<rma_key_info*>(rma_write_keys[r].get());
      if (remote_key_info->length < static_cast<uint64_t>(message_size))
      {
        LIBFATBAT_ERROR(rdmawritetest_log, "rank {} got invalid RMA key length {} from rank {}",
            rank, remote_key_info->length, r);
        return EXIT_FAILURE;
      }
      uint64_t remote_addr = remote_rma_addr_value(controller, *remote_key_info);
      comm.write(local_source_buffers[r], message_size, r, remote_addr, remote_key_info->remote_key,
          nullptr);
      LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} -> rank {} key {:#08x}", "fi_write posted",
          rank, r, remote_key_info->remote_key);
    }

    // Wait for all data writes to complete
    uint32_t const data_write_target = static_cast<uint32_t>(controller.writes_posted_);
    wait_for_write_completions(controller, data_write_target);
    LIBFATBAT_INFO(
        rdmawritetest_log, "{:<20} rank {} iteration {}", "data writes complete", rank, it);

    // Busy-wait until all local target buffers contain the expected peer data.
    auto const wait_start = std::chrono::steady_clock::now();
    while (true)
    {
      bool all = true;
      for (int r = 0; r < static_cast<int>(size); ++r)
      {
        if (r == static_cast<int>(rank)) continue;
        auto* data = static_cast<uint8_t const*>(rma_target_buffers[r].get());
        uint8_t const expected = static_cast<uint8_t>(r + 17);
        bool buffer_ready = true;
        for (std::size_t i = 0; i < static_cast<std::size_t>(message_size); ++i)
        {
          if (data[i] != expected)
          {
            buffer_ready = false;
            break;
          }
        }
        if (!buffer_ready)
        {
          all = false;
          break;
        }
      }
      if (all) break;
      if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(10))
      {
        LIBFATBAT_ERROR(rdmawritetest_log, "rank {} timeout waiting for target buffers", rank);
        for (int r = 0; r < static_cast<int>(size); ++r)
        {
          if (r == static_cast<int>(rank)) continue;
          auto* data = static_cast<uint8_t const*>(rma_target_buffers[r].get());
          uint8_t const expected = static_cast<uint8_t>(r + 17);
          std::size_t mismatch = 0;
          for (; mismatch < static_cast<std::size_t>(message_size); ++mismatch)
          {
            if (data[mismatch] != expected) { break; }
          }
          LIBFATBAT_ERROR(rdmawritetest_log,
              "  buffer {} first mismatch at {} value {} expected {}", r, mismatch,
              mismatch < static_cast<std::size_t>(message_size) ? data[mismatch] : expected,
              expected);
        }
        return EXIT_FAILURE;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    LIBFATBAT_INFO(rdmawritetest_log, "{:<20} rank {} all target buffers updated for iter {}",
        "buffer complete", rank, it);

    // Validate buffers
    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
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
  return 0;
}
