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
#include <cstdio>
#include <thread>
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
MAKE_LOGGER(srbench_log, "SendRecvBench")

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
    LIBFATBAT_ERROR(srbench_log, "iterations must be > 0");
    return EXIT_FAILURE;
  }
  if (min_shift > max_shift)
  {
    LIBFATBAT_ERROR(srbench_log, "min-shift must be <= max-shift");
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

  if (size != 2)
  {
    if (rank == 0) { LIBFATBAT_ERROR(srbench_log, "This benchmark requires exactly 2 ranks."); }
    pmi.fence();
    pmi.finalize_PMI();
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  // memory pinning utility - hwmalloc needs to know which library we are using to pin memory
  memory_context c(&controller);
  memory_context::heap_type heap(&c);

  if (rank == 0)
  {
    std::printf("# send/recv bandwidth benchmark\n");
    std::printf("# iterations=%zu min_shift=%zu max_shift=%zu warmup_iterations=%d\n", iterations,
        min_shift, max_shift, 1);
    std::printf("%-12s%-14s%-14s%-16s%-20s%-20s\n", "bytes", "iters", "time_ms", "msg_rate_M/s",
        "one_way_MB/s", "bi_dir_MB/s");
  }

  {
    poller_guard pg(&controller, rank, 2);

    for (std::size_t bitshift = min_shift; bitshift <= max_shift; ++bitshift)
    {
      uint32_t const base_tag = static_cast<uint32_t>(bitshift);
      std::size_t const msg_size = (std::size_t{1} << bitshift);
      rank_type const peer = static_cast<rank_type>(1 - rank);
      std::size_t const warmup_iterations = 1;
      std::size_t const max_chunk =
          static_cast<std::size_t>(communicator::max_callback_queue_size_);

      auto send_buffer = heap.allocate(msg_size, 0);
      auto recv_buffer = heap.allocate(msg_size, 0);

      std::fill((char*) (send_buffer.get()), (char*) (send_buffer.get()) + msg_size,
          static_cast<uint8_t>(base_tag & 0xff));
      std::fill((char*) (recv_buffer.get()), (char*) (recv_buffer.get()) + msg_size,
          static_cast<uint8_t>(0xff));

      auto post_send_recv_iterations = [&](std::size_t num_iterations) {
        // The communicator request cache is finite and not recycled on this path,
        // so run in bounded chunks using a fresh communicator per chunk.
        std::size_t remaining = num_iterations;
        while (remaining > 0)
        {
          std::size_t const chunk = std::min(remaining, max_chunk);
          communicator comm(&controller, rank, size);
          for (std::size_t i = 0; i < chunk; ++i)
          {
            uint32_t const tag = base_tag;
            comm.recv(recv_buffer, msg_size, fi_addr_t(peer), tag, nullptr);
            comm.send(send_buffer, msg_size, fi_addr_t(peer), tag, nullptr);
          }
          remaining -= chunk;
        }
      };

      pmi.fence();
      uint32_t const warmup_sends_posted_before = (uint32_t) controller.sends_posted_;
      uint32_t const warmup_recvs_posted_before = (uint32_t) controller.recvs_posted_;
      post_send_recv_iterations(warmup_iterations);

      uint32_t const warmup_sends_target =
          warmup_sends_posted_before + static_cast<uint32_t>(warmup_iterations);
      uint32_t const warmup_recvs_target =
          warmup_recvs_posted_before + static_cast<uint32_t>(warmup_iterations);
      while ((uint32_t) controller.sends_complete_ < warmup_sends_target ||
          (uint32_t) controller.recvs_complete_ < warmup_recvs_target)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }

      pmi.fence();
      uint32_t const sends_complete_before = (uint32_t) controller.sends_complete_;
      uint32_t const sends_posted_before = (uint32_t) controller.sends_posted_;
      uint32_t const recvs_complete_before = (uint32_t) controller.recvs_complete_;
      uint32_t const recvs_posted_before = (uint32_t) controller.recvs_posted_;
      auto t0 = std::chrono::steady_clock::now();
      post_send_recv_iterations(iterations);

      uint32_t const sends_target = sends_posted_before + static_cast<uint32_t>(iterations);
      uint32_t const recvs_target = recvs_posted_before + static_cast<uint32_t>(iterations);
      while ((uint32_t) controller.sends_complete_ < sends_target ||
          (uint32_t) controller.recvs_complete_ < recvs_target)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }

      pmi.fence();
      auto t1 = std::chrono::steady_clock::now();

      auto const elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0);
      double const elapsed_s = elapsed.count();
      double const elapsed_ms = elapsed_s * 1.0e3;
      double const total_bytes_one_way =
          static_cast<double>(msg_size) * static_cast<double>(iterations);
      double const one_way_mbps = total_bytes_one_way / elapsed_s / 1.0e6;
      double const bi_dir_mbps = (2.0 * total_bytes_one_way) / elapsed_s / 1.0e6;
      double const msg_rate_mps = static_cast<double>(iterations) / elapsed_s / 1.0e6;

      if (rank == 0)
      {
        std::printf("%-12zu%-14zu%-14.3f%-16.3f%-20.3f%-20.3f\n", msg_size, iterations, elapsed_ms,
            msg_rate_mps, one_way_mbps, bi_dir_mbps);
      }

      uint32_t const sends_done = (uint32_t) controller.sends_complete_ - sends_complete_before;
      uint32_t const recvs_done = (uint32_t) controller.recvs_complete_ - recvs_complete_before;
      if (sends_done != iterations || recvs_done != iterations)
      {
        LIBFATBAT_ERROR(srbench_log,
            "rank {} counter mismatch for msg_size {}: sends {}/{}, recvs {}/{}", rank, msg_size,
            sends_done, iterations, recvs_done, iterations);
        throw std::runtime_error("counter mismatch");
      }

      heap.free(send_buffer);
      heap.free(recv_buffer);
    }
  }

  pmi.fence();
  pmi.finalize_PMI();
  return 0;
}
