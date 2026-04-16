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
inline auto rdmatest_log = libfatbat::log::create("RdmaTest");

// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
  // -------------------------------------------------
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

  if (size < 2)
  {
    LIBFATBAT_ERROR(rdmatest_log, "This test requires exactly 2 ranks.");
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
  // our test will exchange RMA messages of this size
  constexpr int32_t message_size = 1024 * 1024 * 16;    // 16 MB

  // we want to exchange RMA keys with other ranks, so lets create storage
  struct rma_key_info
  {
    void* address;
    uint64_t remote_key;
    uint64_t length;
  };
  // allocate space for an RMA key from each rank and space to read remote data into
  std::vector<memory_context::heap_type::pointer> rma_read_keys;
  std::vector<memory_context::heap_type::pointer> rma_read_buffers;
  // allocate buffers we still store data in and each other rank will read from
  std::vector<memory_context::heap_type::pointer> local_data_keys;
  std::vector<memory_context::heap_type::pointer> local_data_buffers;

  {
    // --------------------------------------------------
    // for each rank, allocate RMA buffers
    // --------------------------------------------------
    for (int i = 0; i < size; i++)
    {
      // this is just a flat array of memory for reading large data blocks into
      auto remote_read_buffer = heap.allocate(message_size, 0);
      rma_read_buffers.push_back(remote_read_buffer);

      // to perform reads, we need to get keys from each rank, we will store them in here
      // allocate a buffer of size rma_key_info for each rank to receive their RMA key info into,
      // and we will also use those buffers to send our RMA key info to other ranks
      auto remote_key_buffer = heap.allocate(sizeof(rma_key_info), 0);
      rma_read_keys.push_back(remote_key_buffer);

      // allocate a flat block of memory (per rank) that others will read from
      auto local_data_buffer = heap.allocate(message_size, 0);
      // fill the buffer with a pattern based on our rank
      std::fill((char*) (local_data_buffer.get()), (char*) (local_data_buffer.get()) + message_size,
          static_cast<uint8_t>(rank));
      local_data_buffers.push_back(local_data_buffer);

      // allocate buffers for rma keys that others can use to read with
      auto local_data_key = heap.allocate(sizeof(rma_key_info), 0);
      rma_key_info info{
          .address = local_data_buffer.handle().get_address(),                     //
          .remote_key = (uint64_t) local_data_buffer.handle().get_remote_key(),    //
          .length = message_size                                                   //
      };
      std::memcpy(local_data_key.get(), &info, sizeof(rma_key_info));
      local_data_keys.push_back(local_data_key);
    }
    LIBFATBAT_INFO(rdmatest_log, "{:<20} RMA buffers :rank {}", "initialized", rank);

    // --------------------------------------------------
    // for each rank, exchange an RMA key
    // --------------------------------------------------
    for (int r = 0; r < size; ++r)
    {
      if (rank != r)
      {
        // receive an rma key from the other rank,
        LIBFATBAT_TRACE(rdmatest_log, "{:<20} rank {} from rank {}", "receiving RMA key", rank, r);
        comm.recv(rma_read_keys[r], sizeof(rma_key_info), r, r, nullptr);

        // and send them our rma key (in any order since we are using tags to match messages)
        LIBFATBAT_TRACE(rdmatest_log, "{:<20} rank {} to rank {}", "sending RMA key", rank, r);
        comm.send(local_data_keys[r], sizeof(rma_key_info), r, rank, nullptr);
      }
    }

    // --------------------------------------------------
    // complete the key exchange by polling for completions until we have received all keys and sent all keys
    // --------------------------------------------------
    while ((uint32_t) controller.sends_complete_ < controller.sends_posted_ ||
        (uint32_t) controller.recvs_complete_ < controller.recvs_posted_)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    LIBFATBAT_INFO(rdmatest_log, "{:<20} RMA keys    :rank {}", "exchanged", rank);
    pmi.fence();

    // --------------------------------------------------
    // do the RMA reads, strictly speaking this is racy as we can perform reads that might overlap with the threads
    // that are checking for completions and invoking callbacks, but since we are only reading into the rma_read_buffers
    // and the callbacks only read from those buffers after the read completes, this should be ok
    // --------------------------------------------------
    for (std::size_t iterations = 0; iterations < 5; ++iterations)
    {
      // for each rank, do an rma read
      for (int r = 0; r < size; ++r)
      {
        if (rank != r)    // we don't read from ourselves
        {
          auto remote_key_info = static_cast<rma_key_info*>(rma_read_keys[r].get());
          // since we are not using FI_MR_VIRT_ADDR we use an offset of 0 for the remote address
          // and rely on the remote key to contain the base address, this is more portable across providers
          auto address = nullptr;
          auto key = remote_key_info->remote_key;
          auto length = remote_key_info->length;
          LIBFATBAT_INFO(rdmatest_log,
              "{:<20} rank {} reading from rank {} with address {:p} key {:#08x} length {:#10x}",
              "RMA read", rank, r, address, key, length);
          if (length != message_size)
          {
            LIBFATBAT_ERROR(rdmatest_log,
                "rank {} received invalid RMA key info length {} from rank {}", rank, length, r);
            throw std::runtime_error("invalid RMA key info length");
          }
          assert(message_size == length);
          comm.read(rma_read_buffers[r], message_size, r, address, key,
              [buf = rma_read_buffers[r].get(), sz = message_size, thisrank = rank](
                  rank_type remote_rank, tag_type tag) {
                verify_buffer(buf, sz, thisrank, remote_rank, "read completion", remote_rank, tag);
              });
        }
      }

      // --------------------------------------------------
      //
      // --------------------------------------------------
      while ((uint32_t) controller.reads_complete_ < controller.reads_posted_)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
      LIBFATBAT_INFO(rdmatest_log, "{:<20} RMA buffers : rank {}", "read complete", rank);
    }
  }
  pmi.fence();
  pmi.finalize_PMI();

  // clean up the pinned memory buffers
  for (auto& buf : rma_read_keys) { heap.free(buf); }
  for (auto& buf : rma_read_buffers) { heap.free(buf); }
  for (auto& buf : local_data_keys) { heap.free(buf); }
  for (auto& buf : local_data_buffers) { heap.free(buf); }

  return 0;
}
