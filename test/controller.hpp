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

#include "libfatbat/controller_base.hpp"
#include "libfatbat/logging.hpp"
//
#include "operation_context.hpp"

// --------------------------------------------------------------------
inline auto ctrl_log = libfatbat::log::create("Ctrl");

// --------------------------------------------------------------------
class test_controller : public libfatbat::controller_base<test_controller, operation_context>
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
  inline int poll_send_queue(fid_cq* tx_cq, void* user_data)
  {
    return static_cast<controller_base*>(this)->poll_send_queue_default(tx_cq, user_data);
  }

  // --------------------------------------------------------------------
  inline int poll_recv_queue(fid_cq* rx_cq, void* user_data)
  {
    return static_cast<controller_base*>(this)->poll_recv_queue_default(rx_cq, user_data);
  }
};
