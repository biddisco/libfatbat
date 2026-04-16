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
#include <thread>
#include <vector>
//
#include "libfatbat/logging.hpp"
//
#include "controller.hpp"

// ------------------------------------------------------------------
inline auto pollhelp_log = libfatbat::log::create("PollHelp");

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
        LIBFATBAT_DEBUG(pollhelp_log,
            "{:<20} rank {}: sends: {}/{}, recvs: {}/{}, reads: {}/{}, writes: {}/{},",
            "Polling loop", rank, (uint32_t) ctrl->sends_complete_, (uint32_t) ctrl->sends_posted_,
            (uint32_t) ctrl->recvs_complete_, (uint32_t) ctrl->recvs_posted_,
            (uint32_t) ctrl->reads_complete_, (uint32_t) ctrl->reads_posted_,
            (uint32_t) ctrl->writes_complete_, (uint32_t) ctrl->writes_posted_);
      }
    }
    LIBFATBAT_DEBUG(pollhelp_log, "{:<20} rank {}", "Polling stopped", rank);
  });
}

// ----------------------------------------------------------------------------
// RAII helper to manage poll thread lifetime.
struct poller_guard
{
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads_;
  std::size_t rank_ = 0;

  explicit poller_guard(test_controller* ctx, std::size_t rank, std::size_t num_threads = 2)
    : rank_(rank)
  {
    threads_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
    {
      threads_.emplace_back(spawn_poll_thread(ctx, stop, rank_));
    }
  }

  ~poller_guard()
  {
    LIBFATBAT_DEBUG(
        pollhelp_log, "{:<20} rank {} ({} threads)", "Polling stopping", rank_, threads_.size());
    stop.store(true, std::memory_order_release);
    for (auto& t : threads_)
    {
      if (t.joinable()) t.join();
    }
    LIBFATBAT_DEBUG(
        pollhelp_log, "{:<20} rank {} ({} threads)", "Polling stopped", rank_, threads_.size());
  }
};
