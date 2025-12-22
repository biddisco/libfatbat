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
//
// namespace libfatbat {

// This struct holds the ready state of a future
// we must also store the context used in libfabric, in case
// a request is cancelled - fi_cancel(...) needs it
struct operation_context : public libfatbat::operation_context_base<operation_context>
{
  //   std::variant<oomph::detail::request_state*, oomph::detail::shared_request_state*> m_req;

  operation_context()
    : libfatbat::operation_context_base<operation_context>()
  {
    // SPDLOG_SCOPE("{} {}", (void*) (this), __func__);
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

// }    // namespace oomph::libfabric
