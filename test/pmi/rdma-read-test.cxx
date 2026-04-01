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

  // we can keep pmi alive to use a fence at the end in case one rank fisnishes before others
  // it seems to be ok if we shut down pmi here
  pmi.fence();
  pmi.finalize_PMI();

  if (size < 2)
  {
    SPDLOG_ERROR("This test requires exactly 2 ranks.");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------
  // memory pinning utility - hwmall needs to know which library we are using to pin memory
  // so we pass our libfatbat::controller into set up the context
  memory_context c(&controller);
  memory_context::heap_type heap(&c);

  // -------------------------------------------------
  // The communicator is one abstraction level higher than the controller
  communicator comm(&controller, rank, size);

  // -------------------------------------------------
  // a dedicated thread for polling completions
  poller_guard pg(&controller, rank);

  // -------------------------------------------------
  // test sending and receiving messages of increasing size
  std::vector<std::tuple<uint32_t, memory_context::heap_type::pointer>> send_recv_buffers;

  // our test will exchange RMA messages of this size
  constexpr int32_t message_size = 1024 * 1024 * 16;    // 16 MB

  // we want to exchange RMA keys with other ranks, so lets create storage
  struct rma_key_info
  {
    void* address;
    uint64_t key;
    uint64_t length;
  };
  // allocate space for an RMA key from each rank and space to read remote data into
  std::vector<memory_context::heap_type::pointer> rma_read_keys;
  std::vector<memory_context::heap_type::pointer> rma_read_buffers;
  // allocate buffers we still store data in and each other rank will read from
  std::vector<memory_context::heap_type::pointer> data_keys;
  std::vector<memory_context::heap_type::pointer> data_buffers;

  // for each rank, allocate RMA buffers
  for (int i = 0; i < size; i++)
  {
    // we will read data into these buffers from some other rank
    auto remote_read_buffer = heap.allocate(message_size, 0);
    rma_read_buffers.push_back(remote_read_buffer);

    // to perform reads, we need to get keys from each rank, we will store them in here
    auto remote_key_buffer = heap.allocate(sizeof(rma_key_info), 0);
    rma_read_keys.push_back(remote_key_buffer);

    // we will fill these buffers with data and others will read from them
    auto local_data_buffer = heap.allocate(message_size, 0);
    // fill the buffer with a pattern based on our rank
    std::fill((char*) (local_data_buffer.get()), (char*) (local_data_buffer.get()) + message_size,
        static_cast<uint8_t>(rank));
    data_buffers.push_back(local_data_buffer);

    // these are the buffers we will use to share our RMA key info with other ranks
    auto data_key = heap.allocate(sizeof(rma_key_info), 0);
    rma_key_info info{.address = data_key.handle().get_address(),
        .key = (uint64_t) data_key.handle().get_local_key(),
        .length = sizeof(rma_key_info)};
    std::memcpy(data_key.get(), &info, sizeof(rma_key_info));
    data_keys.push_back(data_key);
  }

  // for each rank, exchange an RMA key
  for (int r = 0; r < size; ++r)
  {
    if (rank != r)
    {
      SPDLOG_TRACE("{:20} rank {} from rank {}", "receiving RMA key info", rank, r);
      comm.recv(rma_read_keys[r], sizeof(rma_key_info), r, rank, nullptr);

      SPDLOG_TRACE("{:20} rank {} to rank {}", "sending RMA key info", rank, r);
      comm.send(rma_read_keys[rank], sizeof(rma_key_info), r, rank, nullptr);
    }
  }

  std::uint64_t base_tag = 0x0000'0000;
  for (std::size_t iterations = 0; iterations < 1; ++iterations)
  {
    auto send_buffer = heap.allocate(message_size, 0);
    auto recv_buffer = heap.allocate(message_size, 0);

    std::uint64_t tag = base_tag + iterations;
    // store those buffers so we can delete them later
    send_recv_buffers.push_back(std::make_pair(tag, send_buffer));
    send_recv_buffers.push_back(std::make_pair(tag, recv_buffer));

    // Fill send buffer with pattern based on tag
    std::fill((char*) (send_buffer.get()), (char*) (send_buffer.get()) + message_size,
        static_cast<uint8_t>(tag));
    // Fill recv buffer with known invalid pattern
    std::fill((char*) (recv_buffer.get()), (char*) (recv_buffer.get()) + message_size,
        static_cast<uint8_t>(0xff));

    // for each rank, do a send/recv
    for (int r = 0; r < size; ++r)
    {
      if (rank != r)    // we don't send/recv to ourself
      {
        SPDLOG_TRACE("{:20} of size {:#06x} rank {} from rank {} tag {}", "receiving message",
            message_size, rank, r, tag);

        comm.recv(recv_buffer, message_size, r, tag, nullptr);
        SPDLOG_TRACE("{:20} of size {:#06x} rank {} to rank {} tag {}", "sending message",
            message_size, rank, r, tag);
        comm.send(send_buffer, message_size, r, tag, nullptr);
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

  // clean up the pinned memory buffers, first scan them to make sure all contain
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

  return 0;
}
