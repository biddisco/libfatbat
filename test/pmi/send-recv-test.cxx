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
#include <numeric>
#include <thread>
//
#include <boost/program_options.hpp>
#include <hwmalloc/heap.hpp>
//
#include "libfatbat/logging.hpp"
//
#include "communicator.hpp"
#include "controller.hpp"
#include "pmi_helper.hpp"
#include "polling_helper.hpp"
#include "test_utils.hpp"

// ------------------------------------------------------------------
inline auto srtest_log = libfatbat::log::create("SendRecv");

// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
  libfatbat::log::init_from_env();

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
    LIBFATBAT_ERROR(srtest_log, "This test requires exactly 2 ranks.");
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
      for (int remote_rank = 0; remote_rank < size; ++remote_rank)
      {
        if (rank != remote_rank)    // we don't send/recv to ourself
        {
          LIBFATBAT_TRACE(srtest_log, "{:<20} of size {:#06x} rank {} from rank {} tag {}",
              "posting receive", msg_size, rank, remote_rank, tag);
          comm.recv(recv_buffer, msg_size, fi_addr_t(remote_rank), tag,
              [buf = recv_buffer.get(), msg_size, rank, remote_rank, tag](
                  rank_type r, tag_type /*tag*/) {
                verify_buffer(buf, msg_size, rank, tag, "recv completion", r, tag);
              });

          LIBFATBAT_TRACE(srtest_log, "{:<20} of size {:#06x} rank {} to rank {} tag {}",
              "posting send", msg_size, rank, remote_rank, tag);
          comm.send(send_buffer, msg_size, fi_addr_t(remote_rank), tag,
              [buf = send_buffer.get(), msg_size, rank, remote_rank, tag](
                  rank_type r, tag_type /*tag*/) {
                verify_buffer(buf, msg_size, rank, tag, "send completion", r, tag);
              });
        }
      }
    }

    while (controller.sends_complete_ < controller.sends_posted_ ||
        controller.recvs_complete_ < controller.recvs_posted_)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      LIBFATBAT_DEBUG(srtest_log,
          "{:<20} rank {}: sends: {}/{}, recvs: {}/{}, reads: {}/{}, writes: {}/{},",
          "Polling wait", rank, (uint32_t) controller.sends_complete_,
          (uint32_t) controller.sends_posted_, (uint32_t) controller.recvs_complete_,
          (uint32_t) controller.recvs_posted_, (uint32_t) controller.reads_complete_,
          (uint32_t) controller.reads_posted_, (uint32_t) controller.writes_complete_,
          (uint32_t) controller.writes_posted_);
    }
    LIBFATBAT_DEBUG(srtest_log, "{:<20} rank {}", "Exiting polling scope", rank);
  }

  // clean up the pinned memory buffers, first scaan them to make sure all contain
  // the expected tag value - the send buffers were filled by us with tags, the
  // recv buffers should have been filled by the sending rank with the same tag
  for (auto& [tag, buf] : send_recv_buffers)
  {
    LIBFATBAT_DEBUG(srtest_log, "{:<20} rank {} tag {}", "Freeing buffer", rank, tag);
    heap.free(buf);
  }

  LIBFATBAT_DEBUG(srtest_log, "{:<20} rank {}", "Exiting", rank);
  return 0;
}
