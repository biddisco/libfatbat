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

//
#include "libfatbat/operation_context_base.hpp"

using rank_type = std::uint64_t;
using tag_type = std::uint64_t;
using request_callback_type = std::move_only_function<void(rank_type, tag_type)>;

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
    // SPDLOG_SCOPE("{} {}", (void*) (this), __func__);
  }

  // --------------------------------------------------------------------
  void invoke_cb()
  {
    SPDLOG_SCOPE("{} {}", (void*) (this), __func__);
    if (m_callback) m_callback(0, 0);
  }

  // --------------------------------------------------------------------
  // When a completion returns FI_ECANCELED, this is called
  inline int handle_cancelled()
  {
    SPDLOG_SCOPE("{} {}", (void*) (this), __func__);
    return 0;
  }

  // --------------------------------------------------------------------
  // Called when a tagged recv completes
  inline int handle_tagged_recv_completion_impl(void* user_data)
  {
    SPDLOG_SCOPE("{} {} user_data {}", (void*) (this), __func__, user_data);
    return 0;
  }

  // --------------------------------------------------------------------
  // Called when a tagged send completes
  inline int handle_tagged_send_completion_impl(void* user_data)
  {
    SPDLOG_SCOPE("{} {} user_data {}", (void*) (this), __func__, user_data);
    return 0;
  }
};
