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
#include "test/operation_context.hpp"

// --------------------------------------------------------------------
MAKE_LOGGER(ctrl_log, "Ctrl")

// --------------------------------------------------------------------
class test_controller : public libfatbat::controller_base<test_controller, operation_context>
{
  public:
  // --------------------------------------------------------------------
  void initialize_derived(
      std::string const& provider, size_t rank, size_t size, std::size_t threads)
  {
  }

  // --------------------------------------------------------------------
  constexpr fi_threading threadlevel_flags()
  {
#if defined(LIBFATBAT_HAVE_PROVIDER_GNI) || defined(LIBFATBAT_HAVE_PROVIDER_LNX)
    return FI_THREAD_ENDPOINT;
#else
    return FI_THREAD_SAFE;
#endif
  }

  // --------------------------------------------------------------------
  constexpr std::int64_t memory_registration_mode_flags()
  {
    std::int64_t base_flags = FI_MR_ALLOCATED;    // | FI_MR_VIRT_ADDR | FI_MR_PROV_KEY;
    base_flags = base_flags | FI_MR_LOCAL;

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    base_flags = base_flags | FI_MR_HMEM;
#endif

#if defined(LIBFATBAT_HAVE_PROVIDER_CXI)
    return base_flags | FI_MR_ENDPOINT;

#elif defined(LIBFATBAT_HAVE_PROVIDER_EFA)
    return base_flags | FI_MR_MMU_NOTIFY | FI_MR_ENDPOINT;
#else
    return base_flags;
#endif
  }

  // --------------------------------------------------------------------
  uint64_t caps_flags(uint64_t /*available_flags*/) const
  {
    uint64_t flags_required = FI_TAGGED;
#ifndef LIBFATBAT_HAVE_PROVIDER_LNX
    flags_required |= FI_MSG | FI_TAGGED | FI_RECV | FI_SEND | FI_RMA | FI_READ | FI_WRITE |
        FI_REMOTE_READ | FI_REMOTE_WRITE;
# ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    flags_required |= FI_HMEM;
# endif
#endif
    return flags_required;
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
