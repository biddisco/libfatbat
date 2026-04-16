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

#include <cstdint>
#include <functional>
//
#include "libfatbat/logging.hpp"
#include "libfatbat/operation_context_base.hpp"

using rank_type = std::uint64_t;
using tag_type = std::uint64_t;
using request_callback_type = std::move_only_function<void(rank_type, tag_type)>;

// ------------------------------------------------------------------
inline auto ctxt_log = libfatbat::log::create("Context");

// --------------------------------------------------------------------
// we are not supporting cacellation for now
// --------------------------------------------------------------------
struct operation_context : public libfatbat::operation_context_base<operation_context>
{
  // when the operation completes, this callback is invoked to trigger user defined actions
  request_callback_type m_callback;

  // --------------------------------------------------------------------
  operation_context()
    : libfatbat::operation_context_base<operation_context>()
    , m_callback(nullptr)
  {
    // LIBFATBAT_SCOPE("{} {}", (void*) (this), __func__);
  }

  // --------------------------------------------------------------------
  inline void invoke_cb()
  {
    LIBFATBAT_SCOPE(ctxt_log, "{} {}", (void*) (this), __func__);
    if (m_callback) m_callback(0, 0);
  }

  // --------------------------------------------------------------------
  // When a completion returns FI_ECANCELED, this is called
  inline int handle_cancelled()
  {
    LIBFATBAT_SCOPE(ctxt_log, "{} {}", (void*) (this), __func__);
    invoke_cb();
    return 1;
  }

  // --------------------------------------------------------------------
  // Called when a tagged recv completes
  inline int handle_tagged_recv_completion_impl(void* user_data)
  {
    LIBFATBAT_SCOPE(ctxt_log, "{} {} user_data {}", (void*) (this), __func__, user_data);
    invoke_cb();
    return 1;
  }

  // --------------------------------------------------------------------
  // Called when a tagged send completes
  inline int handle_tagged_send_completion_impl(void* user_data)
  {
    LIBFATBAT_SCOPE(ctxt_log, "{} {} user_data {}", (void*) (this), __func__, user_data);
    invoke_cb();
    return 1;
  }

  // --------------------------------------------------------------------
  // Called when an RMA read completes
  inline int handle_rma_read_completion_impl()
  {
    LIBFATBAT_SCOPE(ctxt_log, "{} {}", (void*) (this), __func__);
    invoke_cb();
    return 1;
  }

  // --------------------------------------------------------------------
  // Called when an RMA write completes
  inline int handle_rma_write_completion_impl()
  {
    LIBFATBAT_SCOPE(ctxt_log, "{} {}", (void*) (this), __func__);
    invoke_cb();
    return 1;
  }
};
