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
#include <hwmalloc/device.hpp>
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
MAKE_LOGGER(rdmabench_log, "RdmaBench")

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
      po::value<std::size_t>()->default_value(20), "Maximum message-size shift (size = 1<<shift)")(
      "gpu", "Use GPU memory for read source and destination buffers")("gpu-device",
      po::value<int>()->default_value(0), "GPU device id used when --gpu is set");

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
  bool const use_gpu = vm.count("gpu") > 0;
  int const gpu_device = vm["gpu-device"].as<int>();

  if (iterations == 0)
  {
    LIBFATBAT_ERROR(rdmabench_log, "iterations must be > 0");
    return EXIT_FAILURE;
  }
  if (min_shift > max_shift)
  {
    LIBFATBAT_ERROR(rdmabench_log, "min-shift must be <= max-shift");
    return EXIT_FAILURE;
  }
  if (gpu_device < 0)
  {
    LIBFATBAT_ERROR(rdmabench_log, "gpu-device must be >= 0");
    return EXIT_FAILURE;
  }
#ifndef LIBFATBAT_HAVE_GPU_SUPPORT
  if (use_gpu)
  {
    LIBFATBAT_ERROR(rdmabench_log, "--gpu requested but GPU support is not enabled");
    return EXIT_FAILURE;
  }
#endif

  // -------------------------------------------------
  // we need these for basic control
  std::size_t rank, size;
  std::size_t const nthreads = 2;    // any nthreads>1 triggers thread safety code paths
  test_controller controller;
  pmi_helper pmi;

  // -------------------------------------------------
  // initialize PMI and libfatbat controller
  std::tie(rank, size) = pmi.init_PMI(attach_debugger);
  controller.initialize(LIBFATBAT_HAVE_PROVIDER, rank, size, nthreads);
  pmi.boot_PMI(&controller);

  if (size < 2)
  {
    if (rank == 0) { LIBFATBAT_ERROR(rdmabench_log, "This benchmark requires at least 2 ranks."); }
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
    #ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    rma_read_buffers[r] = use_gpu ? heap.allocate(max_message_size, 0, gpu_device) : heap.allocate(max_message_size, 0);
    #else
    rma_read_buffers[r] = heap.allocate(max_message_size, 0);
    #endif
    rma_read_keys[r] = heap.allocate(sizeof(rma_key_info), 0);

    #ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    local_data_buffers[r] = use_gpu ? heap.allocate(max_message_size, 0, gpu_device) : heap.allocate(max_message_size, 0);
    #else
    local_data_buffers[r] = heap.allocate(max_message_size, 0);
    #endif
    std::fill((char*) (local_data_buffers[r].get()),
        (char*) (local_data_buffers[r].get()) + max_message_size, static_cast<uint8_t>(rank));
  #ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    if (use_gpu && local_data_buffers[r].on_device())
    {
      hwmalloc::memcpy_to_device(
        local_data_buffers[r].device_ptr(), local_data_buffers[r].get(), max_message_size);
    }
  #endif

    local_data_keys[r] = heap.allocate(sizeof(rma_key_info), 0);
  #ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    auto const& key_handle = (use_gpu && local_data_buffers[r].on_device()) ? local_data_buffers[r].device_handle() : local_data_buffers[r].handle();
  #else
    auto const& key_handle = local_data_buffers[r].handle();
  #endif
    rma_key_info info{
      .address = key_handle.get_address(),
      .remote_key = (uint64_t) key_handle.get_remote_key(),
        .length = max_message_size,
    };
    std::memcpy(local_data_keys[r].get(), &info, sizeof(rma_key_info));
  }

  if (rank == 0)
  {
    std::printf("# rdma read benchmark\n");
    std::printf("# iterations=%zu min_shift=%zu max_shift=%zu peers_per_rank=%zu memory=%s gpu_device=%d\n",
      iterations, min_shift, max_shift, size - 1, use_gpu ? "gpu" : "host", gpu_device);
    std::printf("%-12s%-14s%-14s%-14s%-16s%-22s\n", "bytes", "iters", "reads", "time_ms",
        "msg_rate_M/s", "agg_read_MB/s");
  }

  {
    communicator comm_keys(&controller, rank, size);
    poller_guard pg(&controller, rank);

    exchange_rma_keys(comm_keys, controller, local_data_keys, rma_read_keys);
    pmi.fence();

    for (std::size_t bitshift = min_shift; bitshift <= max_shift; ++bitshift)
    {
      std::size_t const msg_size = (std::size_t{1} << bitshift);
      std::size_t const peers = size - 1;
      std::size_t const expected_reads = iterations * peers;
      std::size_t const warmup_iterations = 1;
      uint32_t const max_inflight_reads = 128;
      communicator comm_post(&controller, rank, size);

      auto post_read_iterations = [&](std::size_t num_iterations) {
        for (std::size_t i = 0; i < num_iterations; ++i)
        {
          for (std::size_t r = 0; r < size; ++r)
          {
            if (r == rank) { continue; }

            auto const* remote_key_info = static_cast<rma_key_info*>(rma_read_keys[r].get());
            if (remote_key_info->length < msg_size)
            {
              LIBFATBAT_ERROR(rdmabench_log,
                  "rank {} remote key length {} from rank {} is smaller than msg_size {}", rank,
                  remote_key_info->length, r, msg_size);
              throw std::runtime_error("invalid RMA key length");
            }

            throttle_reads_inflight(controller, max_inflight_reads);
            void* remote_addr = remote_rma_addr_ptr(controller, *remote_key_info, 0);
            comm_post.read(rma_read_buffers[r], msg_size, static_cast<rank_type>(r), remote_addr,
                remote_key_info->remote_key, nullptr);
          }
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
        std::printf("%-12zu%-14zu%-14zu%-14.3f%-16.3f%-22.3f\n", msg_size, iterations, total_reads,
            elapsed_ms, msg_rate_mps, agg_read_mbps);
      }

      uint32_t const reads_done = (uint32_t) controller.reads_complete_ - reads_complete_before;
      if (reads_done != expected_reads)
      {
        LIBFATBAT_ERROR(rdmabench_log, "rank {} read counter mismatch for msg_size {}: reads {}/{}",
            rank, msg_size, reads_done, expected_reads);
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
