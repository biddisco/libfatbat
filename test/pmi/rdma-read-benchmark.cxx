/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
//
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <hwmalloc/heap.hpp>
//
#include "libfatbat/logging.hpp"
//
#include "../communicator.hpp"
#include "../polling_helper.hpp"
#include "../test_controller.hpp"

struct rma_key_info
{
  void* address;
  uint64_t remote_key;
  uint64_t length;
};

// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
  // -------------------------------------------------
  // define log level settings when enabled
#if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::warn);
#endif

  namespace po = boost::program_options;
  po::options_description desc("Options");
  desc.add_options()("help,h", "Show help")("debug", "Enable debug mode")("iterations,i",
      po::value<std::size_t>()->default_value(1000), "Number of iterations per message size")(
      "min-shift", po::value<std::size_t>()->default_value(0),
      "Minimum message-size shift (size = 1<<shift)")("max-shift",
      po::value<std::size_t>()->default_value(20), "Maximum message-size shift (size = 1<<shift)");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help") > 0)
  {
    std::cout << desc << std::endl;
    return EXIT_SUCCESS;
  }

  bool const attach_debugger = vm.count("debug") > 0;
  std::size_t const iterations = vm["iterations"].as<std::size_t>();
  std::size_t const min_shift = vm["min-shift"].as<std::size_t>();
  std::size_t const max_shift = vm["max-shift"].as<std::size_t>();

  if (iterations == 0)
  {
    SPDLOG_ERROR("iterations must be > 0");
    return EXIT_FAILURE;
  }
  if (min_shift > max_shift)
  {
    SPDLOG_ERROR("min-shift must be <= max-shift");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  // we need these for basic control
  std::size_t rank, size;
  std::size_t const nthreads = 2;    // any nthreads>1 triggers thread safety code paths
  test_controller controller;
  pmi_helper pmi;

  // -------------------------------------------------
  // initialize PMI and libfatbat controller
  std::tie(rank, size) = pmi.init_PMI(attach_debugger);
  controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, nthreads);
  pmi.boot_PMI(&controller);

  if (size < 2)
  {
    if (rank == 0) { SPDLOG_ERROR("This benchmark requires at least 2 ranks."); }
    pmi.fence();
    pmi.finalize_PMI();
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  // memory pinning utility - hwmalloc needs to know which library we are using to pin memory
  memory_context c(&controller);
  memory_context::heap_type heap(&c);

  std::size_t const max_message_size = (std::size_t{1} << max_shift);

  // One buffer per rank for key exchange and source/destination data.
  std::vector<memory_context::heap_type::pointer> rma_read_keys(size);
  std::vector<memory_context::heap_type::pointer> rma_read_buffers(size);
  std::vector<memory_context::heap_type::pointer> local_data_keys(size);
  std::vector<memory_context::heap_type::pointer> local_data_buffers(size);

  for (std::size_t r = 0; r < size; ++r)
  {
    rma_read_buffers[r] = heap.allocate(max_message_size, 0);
    rma_read_keys[r] = heap.allocate(sizeof(rma_key_info), 0);

    local_data_buffers[r] = heap.allocate(max_message_size, 0);
    std::fill((char*) (local_data_buffers[r].get()),
        (char*) (local_data_buffers[r].get()) + max_message_size, static_cast<uint8_t>(rank));

    local_data_keys[r] = heap.allocate(sizeof(rma_key_info), 0);
    rma_key_info info{
        .address = local_data_buffers[r].handle().get_address(),
        .remote_key = (uint64_t) local_data_buffers[r].handle().get_remote_key(),
        .length = max_message_size,
    };
    std::memcpy(local_data_keys[r].get(), &info, sizeof(rma_key_info));
  }

  if (rank == 0)
  {
    fmt::print("# rdma read benchmark\n");
    fmt::print("# iterations={} min_shift={} max_shift={} peers_per_rank={}\n", iterations,
        min_shift, max_shift, size - 1);
    fmt::print("{:<12}{:<14}{:<14}{:<14}{:<16}{:<22}\n", "bytes", "iters", "reads", "time_ms",
        "msg_rate_M/s", "agg_read_MB/s");
  }

  {
    communicator comm_keys(&controller, rank, size);
    poller_guard pg(&controller, rank);

    // --------------------------------------------------
    // Exchange RMA keys once.
    // --------------------------------------------------
    for (std::size_t r = 0; r < size; ++r)
    {
      if (r == rank) { continue; }
      comm_keys.recv(rma_read_keys[r], sizeof(rma_key_info), static_cast<rank_type>(r),
          static_cast<tag_type>(r), nullptr);
      comm_keys.send(local_data_keys[r], sizeof(rma_key_info), static_cast<rank_type>(r),
          static_cast<tag_type>(rank), nullptr);
    }

    while ((uint32_t) controller.sends_complete_ < (uint32_t) controller.sends_posted_ ||
        (uint32_t) controller.recvs_complete_ < (uint32_t) controller.recvs_posted_)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    pmi.fence();

    for (std::size_t bitshift = min_shift; bitshift <= max_shift; ++bitshift)
    {
      std::size_t const msg_size = (std::size_t{1} << bitshift);
      std::size_t const peers = size - 1;
      std::size_t const expected_reads = iterations * peers;
      std::size_t const warmup_iterations = 1;
      std::size_t const max_chunk = static_cast<std::size_t>(max_completions_array_limit_ / peers);
      std::size_t const chunk_limit = std::max<std::size_t>(1, max_chunk);

      auto post_read_iterations = [&](std::size_t num_iterations) {
        std::size_t remaining = num_iterations;
        while (remaining > 0)
        {
          std::size_t const chunk = std::min(remaining, chunk_limit);
          // Request contexts are cached per communicator on this path, so use bounded chunks
          // with a fresh communicator instance to keep cache growth under control.
          communicator comm_chunk(&controller, rank, size);
          for (std::size_t i = 0; i < chunk; ++i)
          {
            for (std::size_t r = 0; r < size; ++r)
            {
              if (r == rank) { continue; }

              auto const* remote_key_info = static_cast<rma_key_info*>(rma_read_keys[r].get());
              if (remote_key_info->length < msg_size)
              {
                SPDLOG_ERROR(
                    "rank {} remote key length {} from rank {} is smaller than msg_size {}", rank,
                    remote_key_info->length, r, msg_size);
                throw std::runtime_error("invalid RMA key length");
              }

              // Use zero offset without FI_MR_VIRT_ADDR and rely on remote key base.
              void* remote_addr = nullptr;
              comm_chunk.read(rma_read_buffers[r], msg_size, static_cast<rank_type>(r), remote_addr,
                  remote_key_info->remote_key, nullptr);
            }
          }
          remaining -= chunk;
        }
      };

      pmi.fence();
      uint32_t const warmup_reads_posted_before = (uint32_t) controller.reads_posted_;
      post_read_iterations(warmup_iterations);

      uint32_t const warmup_reads_target =
          warmup_reads_posted_before + static_cast<uint32_t>(warmup_iterations * peers);
      while ((uint32_t) controller.reads_complete_ < warmup_reads_target)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }

      pmi.fence();
      uint32_t const reads_complete_before = (uint32_t) controller.reads_complete_;
      uint32_t const reads_posted_before = (uint32_t) controller.reads_posted_;
      auto t0 = std::chrono::steady_clock::now();
      post_read_iterations(iterations);

      uint32_t const reads_target = reads_posted_before + static_cast<uint32_t>(expected_reads);
      while ((uint32_t) controller.reads_complete_ < reads_target)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }

      pmi.fence();
      auto t1 = std::chrono::steady_clock::now();

      auto const elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0);
      double const elapsed_s = elapsed.count();
      double const elapsed_ms = elapsed_s * 1.0e3;
      std::size_t const total_reads = expected_reads * size;
      double const aggregate_bytes = static_cast<double>(msg_size) *
          static_cast<double>(expected_reads) * static_cast<double>(size);
      double const agg_read_mbps = aggregate_bytes / elapsed_s / 1.0e6;
      double const msg_rate_mps = static_cast<double>(total_reads) / elapsed_s / 1.0e6;

      if (rank == 0)
      {
        fmt::print("{:<12}{:<14}{:<14}{:<14.3f}{:<16.3f}{:<22.3f}\n", msg_size, iterations,
            total_reads, elapsed_ms, msg_rate_mps, agg_read_mbps);
      }

      uint32_t const reads_done = (uint32_t) controller.reads_complete_ - reads_complete_before;
      if (reads_done != expected_reads)
      {
        SPDLOG_ERROR("rank {} read counter mismatch for msg_size {}: reads {}/{}", rank, msg_size,
            reads_done, expected_reads);
        throw std::runtime_error("counter mismatch");
      }
    }
  }

  pmi.fence();
  pmi.finalize_PMI();

  for (auto& buf : rma_read_keys) { heap.free(buf); }
  for (auto& buf : rma_read_buffers) { heap.free(buf); }
  for (auto& buf : local_data_keys) { heap.free(buf); }
  for (auto& buf : local_data_buffers) { heap.free(buf); }

  return 0;
}
