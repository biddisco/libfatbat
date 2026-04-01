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
#include "../polling_helper.hpp"
#include "../test_controller.hpp"

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

  // we can keep pmi alive to use a fence at the end in case one rank finishes before others
  // it seems to be ok if we shut down pmi here
  pmi.fence();
  pmi.finalize_PMI();

  if (size < 2)
  {
    SPDLOG_ERROR("This test requires exactly 2 ranks.");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  // memory pinning utility - hwmalloc needs to know which library we are using to pin memory
  // so we pass our libfatbat::controller in to setup the context
  memory_context c(&controller);
  memory_context::heap_type heap(&c);

  // -------------------------------------------------
  // The communicator is one abstraction level higher than the controller
  communicator comm(&controller, rank, size);

  // -------------------------------------------------
  // test sending and receiving messages of increasing size
  std::vector<std::tuple<uint32_t, memory_context::heap_type::pointer>> send_recv_buffers;

  {
    // -------------------------------------------------
    // a dedicated thread for polling completions
    poller_guard pg(&controller, rank);

    for (std::size_t bitshift = 0; bitshift < 20; ++bitshift)
    {
      // just use a simple tag based on the bitshift
      uint32_t tag = bitshift;
      std::size_t msg_size = 1 << bitshift;

      auto send_buffer = heap.allocate(msg_size, 0);
      auto recv_buffer = heap.allocate(msg_size, 0);
      // store those buffers so we can delete them later
      send_recv_buffers.push_back(std::make_pair(tag, send_buffer));
      send_recv_buffers.push_back(std::make_pair(tag, recv_buffer));

      // Fill send buffer with pattern based on tag
      std::fill((char*) (send_buffer.get()), (char*) (send_buffer.get()) + msg_size,
          static_cast<uint8_t>(tag));
      // Fill recv buffer with known invalid pattern
      std::fill((char*) (recv_buffer.get()), (char*) (recv_buffer.get()) + msg_size,
          static_cast<uint8_t>(0xff));

      // for each rank, do a send/recv
      for (int r = 0; r < size; ++r)
      {
        if (rank != r)    // we don't send/recv to ourself
        {
          SPDLOG_TRACE("{:20} of size {:#06x} rank {} from rank {} tag {}", "receiving message",
              msg_size, rank, r, tag);
          comm.recv(recv_buffer, msg_size, fi_addr_t(r), tag, nullptr /*, nullptr*/);

          SPDLOG_TRACE("{:20} of size {:#06x} rank {} to rank {} tag {}", "sending message",
              msg_size, rank, r, tag);
          comm.send(send_buffer, msg_size, fi_addr_t(r), tag, nullptr /*, nullptr*/);
        }
      }
    }

    while (controller.sends_complete_ < controller.sends_posted_ ||
        controller.recvs_complete_ < controller.recvs_posted_)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      SPDLOG_TRACE("rank {} sends: {}/{}, recvs: {}/{}", rank,    //
          (uint32_t) controller.sends_complete_, (uint32_t) controller.sends_posted_,
          (uint32_t) controller.recvs_complete_, (uint32_t) controller.recvs_posted_);
    }
    SPDLOG_DEBUG("{:20} rank {}", "Exiting polling scope", rank);
  }

  // clean up the pinned memory buffers, first scaan them to make sure all contain
  // the expected tag value - the send buffers were filled by us with tags, the
  // recv buffers should have been filled by the sending rank with the same tag
  for (auto& buf : send_recv_buffers)
  {
    auto tag = std::get<0>(buf);
    auto buffer = std::get<1>(buf);
    auto buffer_ptr = (uint8_t*) (buffer.get());
    SPDLOG_DEBUG("rank {} validating buffer with tag {}", rank, tag);
    bool valid = true;
    for (std::size_t i = 0; i < (1 << tag); ++i)
    {
      if (buffer_ptr[i] != static_cast<uint8_t>(tag))
      {
        valid = false;
        break;
      }
    }
    if (!valid)
    {
      SPDLOG_ERROR("rank {} buffer validation failed for tag {}", rank, tag);
      throw std::runtime_error("buffer validation failed");
    }
    SPDLOG_DEBUG("rank {} freeing buffer with tag {}", rank, tag);
    heap.free(std::get<1>(buf));
  }

  SPDLOG_DEBUG("{:20} rank {}", "Exiting", rank);
  return 0;
}
