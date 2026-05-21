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
#include <cstdio>
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
MAKE_LOGGER(rdmawritedatabench_log, "RdmaWriteDataBench")

// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
  // -------------------------------------------------
  libfatbat::log::init_from_env();

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
    LIBFATBAT_ERROR(rdmawritedatabench_log, "iterations must be > 0");
    return EXIT_FAILURE;
  }
  if (min_shift > max_shift)
  {
    LIBFATBAT_ERROR(rdmawritedatabench_log, "min-shift must be <= max-shift");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  std::size_t rank, size;
  std::size_t const nthreads = 2;    // any nthreads>1 triggers thread safety code paths
  test_controller controller;
  pmi_helper pmi;

  // -------------------------------------------------
  std::tie(rank, size) = pmi.init_PMI(attach_debugger);
  controller.initialize(LIBFATBAT_HAVE_PROVIDER, rank, size, nthreads);
  pmi.boot_PMI(&controller);

  if (size < 2)
  {
    if (rank == 0)
    {
      LIBFATBAT_ERROR(rdmawritedatabench_log, "This benchmark requires at least 2 ranks.");
    }
    pmi.fence();
    pmi.finalize_PMI();
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  memory_context c(&controller);
  memory_context::heap_type heap(&c);

  std::size_t const max_message_size = (std::size_t{1} << max_shift);

  // -------------------------------------------------
  // setup read/write buffers for RDMA testing
  // -------------------------------------------------
  // target_buffers[r] : local buffer that rank r will write into (we expose the RMA key for this)
  // target_keys[r]    : buffer holding the rma_key_info we send to rank r
  // source_buffers[r] : local data buffer we write from when sending to rank r
  // remote_keys[r]    : buffer holding the rma_key_info received from rank r (their target)
  std::vector<memory_context::heap_type::pointer> target_buffers(size);
  std::vector<memory_context::heap_type::pointer> target_keys(size);
  std::vector<memory_context::heap_type::pointer> source_buffers(size);
  std::vector<memory_context::heap_type::pointer> remote_keys(size);

  for (std::size_t r = 0; r < size; ++r)
  {
    // target buffer: where remote ranks will write into
    target_buffers[r] = heap.allocate(max_message_size, 0);
    std::fill((char*) target_buffers[r].get(), (char*) target_buffers[r].get() + max_message_size,
        char(0xEE));

    // key describing our target buffer, to be sent to rank r
    target_keys[r] = heap.allocate(sizeof(rma_key_info), 0);
    rma_key_info info{
        .address = target_buffers[r].handle().get_address(),
        .remote_key = (uint64_t) target_buffers[r].handle().get_remote_key(),
        .length = max_message_size,
    };
    std::memcpy(target_keys[r].get(), &info, sizeof(rma_key_info));

    // source buffer: what we write from when targeting rank r
    source_buffers[r] = heap.allocate(max_message_size, 0);
    std::fill((char*) source_buffers[r].get(), (char*) source_buffers[r].get() + max_message_size,
        static_cast<char>(rank));

    // placeholder to receive rank r's target key
    remote_keys[r] = heap.allocate(sizeof(rma_key_info), 0);
  }

  // -------------------------------------------------
  // prepare to start the benchmark
  // -------------------------------------------------
  if (rank == 0)
  {
    std::printf("# rdma writedata benchmark\n");
    std::printf("# iterations=%zu min_shift=%zu max_shift=%zu peers_per_rank=%zu\n", iterations,
        min_shift, max_shift, size - 1);
    std::printf("%-12s%-14s%-14s%-14s%-16s%-22s\n", "bytes", "iters", "writes", "time_ms",
        "msg_rate_M/s", "agg_write_MB/s");
  }

  // -------------------------------------------------
  // scoped block where polling is active
  // -------------------------------------------------
  {
    communicator comm_keys(&controller, rank, size);
    poller_guard pg(&controller, rank);

    // create a semaphore to signal when we are done too other ranks
    using semaphore_type = semaphore_info<memory_context::heap_type>;
    semaphore_type semaphores(&comm_keys, heap, rank, size);

    exchange_rma_keys(comm_keys, controller, target_keys, remote_keys);
    pmi.fence();

    // --------------------------------------------------
    // Test loop, iterate over messages sizes in powers of 2
    // --------------------------------------------------
    for (std::size_t bitshift = min_shift; bitshift <= max_shift; ++bitshift)
    {
      std::size_t const msg_size = (std::size_t{1} << bitshift);
      std::size_t const peers = size - 1;
      std::size_t const expected_writes = iterations * peers;
      std::size_t const warmup_iterations = 1;
      std::size_t const max_chunk =
          static_cast<std::size_t>(communicator::max_callback_queue_size_ / peers);
      std::size_t const chunk_limit = std::max<std::size_t>(1, max_chunk);

      auto post_writedata_iterations = [&](std::size_t num_iterations) {
        std::size_t remaining = num_iterations;
        while (remaining > 0)
        {
          std::size_t const chunk = std::min(remaining, chunk_limit);
          communicator comm_chunk(&controller, rank, size);
          for (std::size_t i = 0; i < chunk; ++i)
          {
            for (std::size_t r = 0; r < size; ++r)
            {
              if (r == rank) { continue; }

              auto const* remote_key_info = static_cast<rma_key_info*>(remote_keys[r].get());
              if (remote_key_info->length < msg_size)
              {
                LIBFATBAT_ERROR(rdmawritedatabench_log,
                    "rank {} remote key length {} from rank {} is smaller than msg_size {}", rank,
                    remote_key_info->length, r, msg_size);
                throw std::runtime_error("invalid RMA key length");
              }

              uint64_t const remote_addr = remote_rma_addr_value(controller, *remote_key_info);
              comm_chunk.write_data(source_buffers[r], msg_size, static_cast<rank_type>(r),
                  remote_addr, remote_key_info->remote_key, static_cast<uint64_t>(bitshift),
                  nullptr);
            }
          }
          remaining -= chunk;
        }
      };

      pmi.fence();
      uint32_t const warmup_writes_posted_before = (uint32_t) controller.writes_posted_;
      post_writedata_iterations(warmup_iterations);

      uint32_t const warmup_writes_target =
          warmup_writes_posted_before + static_cast<uint32_t>(warmup_iterations * peers);
      while ((uint32_t) controller.writes_complete_ < warmup_writes_target)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }

      pmi.fence();
      uint32_t const writes_complete_before = (uint32_t) controller.writes_complete_;
      uint32_t const writes_posted_before = (uint32_t) controller.writes_posted_;
      auto t0 = std::chrono::steady_clock::now();
      post_writedata_iterations(iterations);

      uint32_t const writes_target = writes_posted_before + static_cast<uint32_t>(expected_writes);
      while ((uint32_t) controller.writes_complete_ < writes_target)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }

      pmi.fence();
      auto t1 = std::chrono::steady_clock::now();

      auto const elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0);
      double const elapsed_s = elapsed.count();
      double const elapsed_ms = elapsed_s * 1.0e3;
      std::size_t const total_writes = expected_writes * size;
      double const aggregate_bytes = static_cast<double>(msg_size) *
          static_cast<double>(expected_writes) * static_cast<double>(size);
      double const agg_write_mbps = aggregate_bytes / elapsed_s / 1.0e6;
      double const msg_rate_mps = static_cast<double>(total_writes) / elapsed_s / 1.0e6;

      if (rank == 0)
      {
        std::printf("%-12zu%-14zu%-14zu%-14.3f%-16.3f%-22.3f\n", msg_size, iterations, total_writes,
            elapsed_ms, msg_rate_mps, agg_write_mbps);
      }

      uint32_t const writes_done = (uint32_t) controller.writes_complete_ - writes_complete_before;
      if (writes_done != expected_writes)
      {
        LIBFATBAT_ERROR(rdmawritedatabench_log,
            "rank {} write counter mismatch for msg_size {}: writes {}/{}", rank, msg_size,
            writes_done, expected_writes);
        throw std::runtime_error("counter mismatch");
      }

      // --------------------------------------------------
      // Post semaphore writes to signal all benchmark writes are complete
      // --------------------------------------------------
      for (std::size_t r = 0; r < size; ++r)
      {
        if (r == rank) { continue; }
        semaphores.signal_completion(bitshift, r);
      }
      uint32_t const write_target = static_cast<uint32_t>(controller.writes_posted_);
      wait_for_write_completions(controller, write_target);

      // Wait for all remote peers to set our local semaphore buffer
      while (true)
      {
        bool all_done = true;
        for (std::size_t r = 0; r < size; ++r)
        {
          if (r == rank) { continue; }
          auto val = semaphores.read_completion(r);
          if (val != bitshift)
          {
            LIBFATBAT_ERROR(rdmawritedatabench_log,
                "{:<20} rank:{:02} has not yet received completion signal {} from rank {}",
                "waiting for peers", rank, bitshift, r);
            all_done = false;
            break;
          }
        }
        if (all_done) { break; }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
  }    // poller_guard scope ends here

  pmi.fence();
  pmi.finalize_PMI();

  for (auto& buf : target_buffers) { heap.free(buf); }
  for (auto& buf : target_keys) { heap.free(buf); }
  for (auto& buf : source_buffers) { heap.free(buf); }
  for (auto& buf : remote_keys) { heap.free(buf); }

  return 0;
}
