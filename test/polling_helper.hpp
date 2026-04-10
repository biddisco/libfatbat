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
#include "libfatbat/logging.hpp"
//
#include "../communicator.hpp"
#include "../test_controller.hpp"

// ----------------------------------------------------------------------------
// Spawns a background thread that polls until stop_flag is set.
inline std::thread spawn_poll_thread(
    test_controller* ctrl, std::atomic<bool>& stop_flag, std::size_t rank)
{
  return std::thread([ctrl, &stop_flag, rank]() {
    while (!stop_flag.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      // TODO: call the progress/poll function(s) here, e.g.:
      auto progress = ctrl->poll_for_work_completions(nullptr);
      if (progress.num() > 0)
      {
        SPDLOG_DEBUG("{:20} rank {}: sends: {}/{}, recvs: {}/{}, reads: {}/{}, writes: {}/{},",
            "Polling loop", rank, (uint32_t) ctrl->sends_complete_, (uint32_t) ctrl->sends_posted_,
            (uint32_t) ctrl->recvs_complete_, (uint32_t) ctrl->recvs_posted_,
            (uint32_t) ctrl->reads_complete_, (uint32_t) ctrl->reads_posted_,
            (uint32_t) ctrl->writes_complete_, (uint32_t) ctrl->writes_posted_);
      }
    }
    SPDLOG_DEBUG("{:20} rank {}", "Polling stopped", rank);
  });
}

// ----------------------------------------------------------------------------
// RAII helper to manage poll thread lifetime.
struct poller_guard
{
  std::atomic<bool> stop{false};
  std::thread thread_;
  std::size_t rank_ = 0;

  explicit poller_guard(test_controller* ctx, std::size_t rank)
    : thread_(spawn_poll_thread(ctx, stop, rank))
    , rank_(rank)
  {
  }

  ~poller_guard()
  {
    SPDLOG_DEBUG("{:20} rank {}", "Polling stopping", rank_);
    stop.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    SPDLOG_DEBUG("{:20} rank {}", "Polling stopped", rank_);
  }
};
