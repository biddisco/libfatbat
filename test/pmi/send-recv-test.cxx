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
#include <thread>
//
#include <boost/program_options.hpp>
#include <hwmalloc/heap.hpp>
//
#include "libfatbat/logging.hpp"
//
#include "../communicator.hpp"
#include "../test_controller.hpp"

// ----------------------------------------------------------------------------
// Spawns a background thread that polls until stop_flag is set.
inline std::thread spawn_poll_thread(test_controller* ctrl, std::atomic<bool>& stop_flag)
{
  return std::thread([&ctrl, &stop_flag]() {
    while (!stop_flag.load(std::memory_order_acquire))
    {
      // std::this_thread::sleep_for(std::chrono::milliseconds(1));
      // TODO: call the progress/poll function(s) here, e.g.:
      // ctrl->poll_for_work_completions(nullptr);
      // SPDLOG_TRACE("{:20}", "Polling loop");
    }
  });
}

// RAII helper to manage poll thread lifetime.
struct poller_guard
{
  std::atomic<bool> stop{false};
  std::thread thread_;

  explicit poller_guard(test_controller* ctx)
    : thread_(spawn_poll_thread(ctx, stop))
  {
  }
  ~poller_guard()
  {
    stop.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }
};

// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
  // -------------------------------------------------
  // define log level settings when enabled
#if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::trace);
#endif

  // -------------------------------------------------
  // define a boost program options parser and setup flags
  namespace po = boost::program_options;
  po::options_description desc("Options");
  desc.add_options()("debug", "Enable debug mode");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  // -------------------------------------------------
  // did the user add "--debug" flag?
  bool attach_debugger = vm.count("debug") > 0;

  // -------------------------------------------------
  // we need these for basic control
  std::size_t rank, size, nthreads = 2;    // any nthreads>1 triggers thread safety code paths
  test_controller controller;
  pmi_helper pmi;

  // -------------------------------------------------
  // initialize PMI and libfatbat controller
  std::tie(rank, size) = pmi.init_PMI(attach_debugger);
  controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, nthreads);
  pmi.boot_PMI(&controller);

  if (size < 2)
  {
    SPDLOG_ERROR("This test requires exactly 2 ranks.");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  // memory pinning utility - hwmall needs to know which library we are using to pin memory
  // so we pass our libfatbat::controller into set up the context
  memory_context c(&controller);
  memory_context::heap_type h(&c);

  // -------------------------------------------------
  // The communicator is one abstraction level higher than the controller
  communicator comm(&controller, rank, size);

  // -------------------------------------------------
  // a dedicated thread for polling completions
  poller_guard pg(&controller);

  // -------------------------------------------------
  // test sending and receiving messages of increasing size
  std::vector<memory_context::heap_type::pointer> send_recv_buffers;

  for (std::size_t bitshift = 0; bitshift < 20; ++bitshift)
  {
    // just use a simple tag based on the bitshift
    uint32_t tag = bitshift;
    std::size_t msg_size = 1 << bitshift;

    auto send_buffer = h.allocate(msg_size, 0);
    auto recv_buffer = h.allocate(msg_size, 0);
    // store those buffers so we can delete them later
    send_recv_buffers.push_back(send_buffer);
    send_recv_buffers.push_back(recv_buffer);

    // Fill send buffer with pattern based on tag
    // std::fill((char*)(send_buffer.get()), (char*)(send_buffer.get()) + msg_size, static_cast<uint8_t>(tag));

    // for each rank, do a send/recv
    for (int r = 0; r < size; ++r)
    {
      if (rank != r)    // we don't send/recv to ourself
      {
        SPDLOG_TRACE("{:20} of size {:#06x} rank {} from rank {} tag {}", "receiving message",
            msg_size, rank, r, tag);
        comm.recv(recv_buffer, msg_size, fi_addr_t(r), tag, nullptr, nullptr);
        SPDLOG_TRACE("{:20} of size {:#06x} rank {} to rank {} tag {}", "sending message", msg_size,
            rank, r, tag);
        comm.send(send_buffer, msg_size, fi_addr_t(r), tag, nullptr, nullptr);
      }
      controller.poll_for_work_completions(nullptr);
    }
    controller.poll_for_work_completions(nullptr);
    SPDLOG_DEBUG("rank {} sends: {}/{}, recvs: {}/{}", rank,    //
        (uint32_t) controller.sends_complete_, (uint32_t) controller.sends_posted_,
        (uint32_t) controller.recvs_complete_, (uint32_t) controller.recvs_posted_);
  }

  while (controller.sends_complete_ < controller.sends_posted_ ||
      controller.recvs_complete_ < controller.recvs_posted_)
  {
    controller.poll_for_work_completions(nullptr);
    SPDLOG_TRACE("rank {} sends: {}/{}, recvs: {}/{}", rank,    //
        (uint32_t) controller.sends_complete_, (uint32_t) controller.sends_posted_,
        (uint32_t) controller.recvs_complete_, (uint32_t) controller.recvs_posted_);
  }

  // clean up the pinned memory buffers
  for (auto& buf : send_recv_buffers) { h.free(buf); }

  pmi.fence();
  pmi.finalize_PMI();
  return 0;
}
