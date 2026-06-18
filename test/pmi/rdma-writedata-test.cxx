/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
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
MAKE_LOGGER(rdmawritedatatest_log, "RdmaWriteDataTest")

namespace {

  uint64_t make_imm_data(uint64_t sender_rank, uint64_t iteration)
  {
    return ((sender_rank & 0xFFFF'FFFFULL) << 32) | (iteration & 0xFFFF'FFFFULL);
  }

}    // namespace

// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
  libfatbat::log::init_from_env();

  namespace po = boost::program_options;
  po::options_description desc("Options");
  desc.add_options()("help,h", "Show help")("debug", "Enable debug mode")(
      "gpu", "Use GPU memory for writedata source buffers")(
      "gpu-device", po::value<int>()->default_value(0), "GPU device id used when --gpu is set");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help") > 0)
  {
    std::cout << desc << std::endl;
    return EXIT_SUCCESS;
  }

  bool attach_debugger = vm.count("debug") > 0;
  bool const use_gpu = vm.count("gpu") > 0;
  int const gpu_device = vm["gpu-device"].as<int>();

  if (gpu_device < 0)
  {
    LIBFATBAT_ERROR(rdmawritedatatest_log, "gpu-device must be >= 0");
    return EXIT_FAILURE;
  }
#ifndef LIBFATBAT_HAVE_GPU_SUPPORT
  if (use_gpu)
  {
    LIBFATBAT_ERROR(rdmawritedatatest_log, "--gpu requested but GPU support is not enabled");
    return EXIT_FAILURE;
  }
#endif

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
    LIBFATBAT_ERROR(rdmawritedatatest_log, "This test requires at least 2 ranks.");
    return EXIT_FAILURE;
  }

  if (!controller.supports_write_data())
  {
    LIBFATBAT_ERROR(rdmawritedatatest_log,
        "Provider does not advertise writedata support (cq_data_size/caps check failed)");
    pmi.fence();
    pmi.finalize_PMI();
    return EXIT_FAILURE;
  }

  memory_context c(&controller);
  memory_context::heap_type heap(&c);
  communicator comm(&controller, rank, size);

  // dedicated background threads for polling completions (both TX and RX CQs)
  poller_guard pg(&controller, rank);

  constexpr int32_t message_size = 1024 * 1024 * 16;
  constexpr std::size_t iterations = 5;

  // other ranks will write to these target buffers using fi_writedata
  std::vector<memory_context::heap_type::pointer> rma_target_buffers;
  std::vector<memory_context::heap_type::pointer> rma_write_keys;
  // we will use these local source buffers as the source for fi_writedata operations
  std::vector<memory_context::heap_type::pointer> local_source_buffers;
  std::vector<memory_context::heap_type::pointer> local_source_keys;

  for (int i = 0; i < static_cast<int>(size); i++)
  {
    // --------------------------------------------------------------------------
    // allocate a target buffer for each peer to write into,
    // and a corresponding RMA key buffer to exchange the address/key info with the peer.
    // --------------------------------------------------------------------------
    auto target_buffer = heap.allocate(message_size, 0);
    std::fill((uint8_t*) target_buffer.get(), (uint8_t*) target_buffer.get() + message_size,
        uint8_t(0xEE));
    rma_target_buffers.push_back(target_buffer);

    auto remote_key_buffer = heap.allocate(sizeof(rma_key_info), 0);
    rma_write_keys.push_back(remote_key_buffer);

    // --------------------------------------------------------------------------
    // allocate a source buffer (per peer) to write from,
    // and a corresponding key buffer to hold the target buffer info for the peer's fi_writedata operation.
    // --------------------------------------------------------------------------
#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    auto source_buffer =
        use_gpu ? heap.allocate(message_size, 0, gpu_device) : heap.allocate(message_size, 0);
#else
    auto source_buffer = heap.allocate(message_size, 0);
#endif

    // --------------------------------------------------------------------------
    // fill the source buffer with the sender's rank for easy verification on the receiver side
    // --------------------------------------------------------------------------
    std::fill((uint8_t*) source_buffer.get(), (uint8_t*) source_buffer.get() + message_size,
        static_cast<uint8_t>(rank));
#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    if (use_gpu && source_buffer.on_device())
    {
      hwmalloc::memcpy_to_device(
          source_buffer.device_ptr(), source_buffer.get(), static_cast<std::size_t>(message_size));
    }
#endif
    local_source_buffers.push_back(source_buffer);

    // rma key info struct to exchange the target buffer address/key with the peer
    auto source_key = heap.allocate(sizeof(rma_key_info), 0);
    rma_key_info info{
        .address = target_buffer.handle().get_address(),
        .remote_key = (uint64_t) target_buffer.handle().get_remote_key(),
        .length = message_size,
    };
    std::memcpy(source_key.get(), &info, sizeof(rma_key_info));
    local_source_keys.push_back(source_key);
  }
  LIBFATBAT_INFO(rdmawritedatatest_log, "{:<20} rank {}", "initialized", rank);

  // each remote rank that is writing into one of our buffers, needs the rma key info for that buffer,
  // we need the rma key info of remote buffers we are writing into
  exchange_rma_keys(comm, controller, local_source_keys, rma_write_keys);
  LIBFATBAT_INFO(rdmawritedatatest_log, "{:<20} rank {}", "key exchange complete", rank);
  pmi.fence();

  std::size_t const peers = size - 1;

  for (std::size_t it = 0; it < iterations; ++it)
  {
    std::mutex remote_cq_data_mutex;
    std::set<uint64_t> seen_senders;
    std::atomic<bool> remote_cq_data_ok{true};
    std::atomic<uint32_t> remote_cq_data_processed{0};
    uint64_t const expected_iter = it + 1;

    // Install a remote CQ data callback to validate the data received from fi_writedata
    // completions and track which peers have completed.
    remote_cq_data_callback_scope callback_scope(controller, [&](uint64_t data) {
      uint64_t const sender = (data >> 32) & 0xFFFF'FFFFULL;
      uint64_t const iter = data & 0xFFFF'FFFFULL;
      if (iter != expected_iter || sender >= size || sender == rank)
      {
        LIBFATBAT_ERROR(rdmawritedatatest_log,
            "Unexpected CQ data value: sender={} iter={} expected_iter={} rank={}", sender, iter,
            expected_iter, rank);
        remote_cq_data_ok.store(false, std::memory_order_relaxed);
      }

      if (remote_cq_data_ok.load(std::memory_order_relaxed))
      {
        std::lock_guard<std::mutex> lock(remote_cq_data_mutex);
        seen_senders.insert(sender);
      }
      remote_cq_data_processed.fetch_add(1, std::memory_order_release);
    });

    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      std::fill((uint8_t*) rma_target_buffers[r].get(),
          (uint8_t*) rma_target_buffers[r].get() + message_size, uint8_t(0xEE));
    }

    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;

      auto* remote_key_info = static_cast<rma_key_info*>(rma_write_keys[r].get());
      if (remote_key_info->length != static_cast<uint64_t>(message_size))
      {
        LIBFATBAT_ERROR(rdmawritedatatest_log, "rank {} got invalid RMA key length {} from rank {}",
            rank, remote_key_info->length, r);
        return EXIT_FAILURE;
      }

      uint64_t const remote_addr = remote_rma_addr_value(controller, *remote_key_info);
      uint64_t const imm_data = make_imm_data(rank, it + 1);
      comm.write_data_delivery(local_source_buffers[r], message_size, r, remote_addr,
          remote_key_info->remote_key, imm_data, nullptr);
      LIBFATBAT_INFO(rdmawritedatatest_log, "{:<20} rank {} -> rank {} key {:#08x} imm {:#016x}",
          "fi_writedata posted", rank, r, remote_key_info->remote_key, imm_data);
    }

    uint32_t const data_write_target = static_cast<uint32_t>(controller.writes_posted_);
    wait_for_write_completions(controller, data_write_target);

    LIBFATBAT_INFO(
        rdmawritedatatest_log, "{:<20} rank {} iteration {}", "writes complete", rank, it);

    // fi_writedata gives us a remote CQ event per peer, so once TX completions and
    // callback processing are both complete there is no separate shutdown semaphore to post.
    uint32_t const expected_events = static_cast<uint32_t>(peers);
    wait_for_counter(remote_cq_data_processed, expected_events);

    if (!remote_cq_data_ok.load(std::memory_order_relaxed)) { return EXIT_FAILURE; }

    pmi.fence();

    {
      std::lock_guard<std::mutex> lock(remote_cq_data_mutex);
      if (seen_senders.size() != peers)
      {
        LIBFATBAT_ERROR(rdmawritedatatest_log,
            "CQ data sender set mismatch: got {} unique senders expected {}", seen_senders.size(),
            peers);
        return EXIT_FAILURE;
      }
    }

    if (!comm.delivery_complete_supported)
    {
      auto const visibility_wait_start = std::chrono::steady_clock::now();
      while (true)
      {
        bool all_visible = true;
        for (int r = 0; r < static_cast<int>(size); ++r)
        {
          if (rank == static_cast<size_t>(r)) continue;
          auto* data = static_cast<uint8_t const*>(rma_target_buffers[r].get());
          uint8_t const expected = static_cast<uint8_t>(r);
          for (std::size_t i = 0; i < static_cast<std::size_t>(message_size); ++i)
          {
            if (data[i] != expected)
            {
              all_visible = false;
              break;
            }
          }
          if (!all_visible) { break; }
        }
        if (all_visible) { break; }
        if (std::chrono::steady_clock::now() - visibility_wait_start > std::chrono::seconds(10))
        {
          LIBFATBAT_ERROR(rdmawritedatatest_log,
              "rank {} timeout waiting for writedata visibility at iteration {}", rank, it);
          return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }

    for (int r = 0; r < static_cast<int>(size); ++r)
    {
      if (rank == static_cast<size_t>(r)) continue;
      verify_buffer(rma_target_buffers[r].get(), message_size, rank, static_cast<uint8_t>(r),
          "writedata completion", r, 0);
    }

    LIBFATBAT_INFO(
        rdmawritedatatest_log, "{:<20} rank {} iteration {}", "writedata complete", rank, it);
    pmi.fence();
  }

  pmi.finalize_PMI();

  for (auto& buf : rma_write_keys) { heap.free(buf); }
  for (auto& buf : rma_target_buffers) { heap.free(buf); }
  for (auto& buf : local_source_keys) { heap.free(buf); }
  for (auto& buf : local_source_buffers) { heap.free(buf); }

  return 0;
}
