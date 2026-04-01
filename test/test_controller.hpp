/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <chrono>

#include "libfatbat/controller_base.hpp"
#include "libfatbat/locality.hpp"
#include "libfatbat/logging.hpp"
//
#include "operation_context.hpp"
#include "pmi_helper.hpp"

// --------------------------------------------------------------------
class test_controller : public libfatbat::controller_base<test_controller>
{
  public:
  // --------------------------------------------------------------------
  void initialize_derived(std::string const& provider, bool rootnode, int size, std::size_t threads)
  {
  }

  // --------------------------------------------------------------------
  constexpr fi_threading threadlevel_flags()
  {
#if defined(HAVE_LIBFATBAT_GNI) || defined(HAVE_LIBFATBAT_LNX)
    return FI_THREAD_ENDPOINT;
#else
    return FI_THREAD_SAFE;
#endif
  }

  // --------------------------------------------------------------------
  uint64_t caps_flags(uint64_t /*available_flags*/) const
  {
    uint64_t flags_required = FI_TAGGED;
#ifndef HAVE_LIBFATBAT_LNX
    flags_required |= FI_MSG | FI_TAGGED | FI_RECV | FI_SEND | FI_RMA | FI_READ | FI_WRITE |
        FI_REMOTE_READ | FI_REMOTE_WRITE;
# if LIBFATBAT_ENABLE_DEVICE
    flags_required |= FI_HMEM;
# endif
#endif
    return flags_required;
  }

  // --------------------------------------------------------------------
  // we do not need to perform any special actions on init (to init address)
  struct fi_info* set_src_dst_addresses(struct fi_info* info, bool tx)
  {    //
    return fi_dupinfo(info);
  }

  // --------------------------------------------------------------------
  int poll_send_queue(fid_cq* tx_cq, void* user_data)
  {
    // SPDLOG_SCOPE("{} tx_cq {} {}", (void*) (this), (void*) (tx_cq), __func__);
#ifdef EXCESSIVE_POLLING_BACKOFF_MICRO_S
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::microseconds>(now - send_poll_stamp).count() <
        EXCESSIVE_POLLING_BACKOFF_MICRO_S)
      return 0;
    send_poll_stamp = now;
#endif
    int ret;
    fi_cq_msg_entry entry[max_completions_array_limit_];
    assert(max_completions_per_poll_ <= max_completions_array_limit_);
    {
      auto lock = try_tx_lock();

      // if we're not threadlocal and didn't get the lock,
      // then another thread is polling now, just exit
      if (!bypass_tx_lock() && !lock.owns_lock()) { return -1; }

      // static auto polling =
      //     NS_DEBUG::cnt_deb<9>.make_timer(1, NS_DEBUG::str<>("poll send queue"));
      // LF_DEB(cnt_deb<9>, timed(polling, hptr(tx_cq)));

      // poll for completions
      {
        ret = fi_cq_read(tx_cq, &entry[0], max_completions_per_poll_);
      }
      // if there is an error, retrieve it
      if (ret == -FI_EAVAIL)
      {
        struct fi_cq_err_entry e = {};
        int err_sz = fi_cq_readerr(tx_cq, &e, 0);
        (void) err_sz;

        // flags might not be set correctly
        if ((e.flags & (FI_MSG | FI_SEND | FI_TAGGED)) != 0)
        {
          SPDLOG_ERROR(
              "{:20} for FI_SEND, len {:#06x}, context {:p}, code {:03}, flags {:016b}, error {}",
              "txcq FI_EAVAIL", static_cast<unsigned long>(e.len), (void*) (e.op_context), e.err,
              e.flags, fi_cq_strerror(tx_cq, e.prov_errno, e.err_data, (char*) e.buf, e.len));
        }
        else if ((e.flags & FI_RMA) != 0)
        {
          SPDLOG_ERROR(
              "{:20} for FI_RMA, len {:#06x}, context {:p}, code {:03}, flags {:016b}, error {}",
              "txcq FI_EAVAIL", static_cast<unsigned long>(e.len), (void*) (e.op_context), e.err,
              e.flags, fi_cq_strerror(tx_cq, e.prov_errno, e.err_data, (char*) e.buf, e.len));
        }
        operation_context* handler = reinterpret_cast<operation_context*>(e.op_context);
        handler->handle_error(e);
        return 0;
      }
    }
    //
    // exit possibly locked region and process each completion
    //
    if (ret > 0)
    {
      std::array<char, 1024> buf;
      int processed = 0;
      for (int i = 0; i < ret; ++i)
      {
        ++sends_complete_;
        SPDLOG_DEBUG("{:20} {:02}, txcq_flags {}, ({:03}), context {:p}, length {:#06x}",
            "Completion", i,
            fi_tostr_r(buf.data(), buf.size(), &entry[i].flags, FI_TYPE_CQ_EVENT_FLAGS),
            entry[i].flags, (void*) (entry[i].op_context), entry[i].len);
        if ((entry[i].flags & (FI_TAGGED | FI_SEND | FI_MSG)) != 0)
        {
          SPDLOG_DEBUG("{:20} {:02}, txcq tagged send completion, context {:p}", "Completion", i,
              (void*) (entry[i].op_context));

          operation_context* handler = reinterpret_cast<operation_context*>(entry[i].op_context);
          processed += handler->handle_tagged_send_completion(user_data);
        }
        else
        {
          SPDLOG_ERROR("Received an unknown txcq completion, flags {:016b}", entry[i].flags);
          std::terminate();
        }
      }
      return processed;
    }
    else if (ret == 0 || ret == -FI_EAGAIN)
    {
      // do nothing, we will try again on the next check
    }
    else
    {
      SPDLOG_ERROR("{:20} unknown error in completion txcq read", "Completion");
    }
    return 0;
  }

  // --------------------------------------------------------------------
  int poll_recv_queue(fid_cq* rx_cq, void* user_data)
  {
    // SPDLOG_SCOPE("{} rx_cq {} {}", (void*) (this), (void*) (rx_cq), __func__);
#ifdef EXCESSIVE_POLLING_BACKOFF_MICRO_S
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::microseconds>(now - recv_poll_stamp).count() <
        EXCESSIVE_POLLING_BACKOFF_MICRO_S)
      return 0;
    recv_poll_stamp = now;
#endif
    int ret;
    fi_cq_msg_entry entry[max_completions_array_limit_];
    assert(max_completions_per_poll_ <= max_completions_array_limit_);
    {
      auto lock = get_rx_lock();

      // if we're not threadlocal and didn't get the lock,
      // then another thread is polling now, just exit
      if (!bypass_rx_lock() && !lock.owns_lock()) { return -1; }

      // static auto polling = NS_DEBUG::cnt_deb<2>.make_timer(1, NS_DEBUG::str<>("poll recv queue"));
      // LF_DEB(cnt_deb<2>, timed(polling, hptr(rx_cq)));

      // poll for completions
      {
        ret = fi_cq_read(rx_cq, &entry[0], max_completions_per_poll_);
      }
      // if there is an error, retrieve it
      if (ret == -FI_EAVAIL)
      {
        // read the full error status
        struct fi_cq_err_entry e = {};
        int err_sz = fi_cq_readerr(rx_cq, &e, 0);
        (void) err_sz;
        // from the manpage 'man 3 fi_cq_readerr'
        if (e.err == FI_ECANCELED)
        {
          SPDLOG_DEBUG("{:20} {:02}, rxcq Cancelled, flags {:#06x}, len {:#06x}, context {:p}",
              "Completion", 0, e.flags, e.len, (void*) (e.op_context));
          // the request was cancelled, we can simply exit
          // as the canceller will have doone any cleanup needed
          operation_context* handler = reinterpret_cast<operation_context*>(e.op_context);
          handler->handle_cancelled();
          return 0;
        }
        else if (e.err != FI_SUCCESS)
        {
          SPDLOG_ERROR(
              "{:20} {:02}, rxcq error, flags {:#06x}, len {:#06x}, context {:p}, error msg {}",
              "Completion", 0, e.flags, e.len, (void*) (e.op_context),
              fi_cq_strerror(rx_cq, e.prov_errno, e.err_data, (char*) e.buf, e.len));
        }
        SPDLOG_SCOPE("{} {}", (void*) (this), __func__);
        operation_context* handler = reinterpret_cast<operation_context*>(e.op_context);
        if (handler) handler->handle_error(e);
        return 0;
      }
    }
    //
    // release the lock and process each completion
    //
    if (ret > 0)
    {
      std::array<char, 1024> buf;
      int processed = 0;
      for (int i = 0; i < ret; ++i)
      {
        ++recvs_complete_;
        SPDLOG_DEBUG("{:20} {:02}, rxcq_flags {}, ({:03}), context {:p}, length {:#06x}",
            "Completion", i,
            fi_tostr_r(buf.data(), buf.size(), &entry[i].flags, FI_TYPE_CQ_EVENT_FLAGS),
            entry[i].flags, (void*) (entry[i].op_context), entry[i].len);
        if ((entry[i].flags & (FI_TAGGED | FI_RECV)) != 0)
        {
          SPDLOG_DEBUG("{:20} {:02}, rxcq tagged recv completion, context {:p}", "Completion", i,
              (void*) (entry[i].op_context));

          operation_context* handler = reinterpret_cast<operation_context*>(entry[i].op_context);
          processed += handler->handle_tagged_recv_completion(user_data);
        }
        else
        {
          SPDLOG_ERROR(
              "{:20} {:02}, received an unknown rxcq completion, flags {:#06x}, context {:p}",
              "Completion", i, entry[i].flags, (void*) (entry[i].op_context));
          std::terminate();
        }
      }
      return processed;
    }
    else if (ret == 0 || ret == -FI_EAGAIN)
    {
      // do nothing, we will try again on the next check
    }
    else
    {
      SPDLOG_ERROR("{:20} unknown error in completion rxcq read", "Completion");
    }
    return 0;
  }
};
