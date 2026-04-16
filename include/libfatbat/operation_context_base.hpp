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

#include <exception>
//
#include <rdma/fi_eq.h>

namespace libfatbat {

  inline auto opctx_log = libfatbat::log::create("opctxtbase");

  // This struct holds the ready state of a future
  // we must also store the context used in libfabric, in case
  // a request is cancelled - fi_cancel(...) needs it
  template <typename Derived>
  struct operation_context_base
  {
private:
    // libfabric requires some space for it's internal bookkeeping
    // so the first member of this struct must be fi_context
    fi_context context_reserved_space;

public:
    operation_context_base()
      : context_reserved_space()
    {
      LIBFATBAT_SCOPE(opctx_log, "{} {}", (void*) (this), __func__);
    }

    // error
    void handle_error(struct fi_cq_err_entry& err)
    {
      static_cast<Derived*>(this)->handle_error_impl(err);
    }
    void handle_error_impl(struct fi_cq_err_entry& err)
    {
      throw libfatbat::fabric_error(err.prov_errno, "Unhandled error in operation context");
      std::terminate();
    }

    void handle_cancelled() { static_cast<Derived*>(this)->handle_cancelled_impl(); }
    void handle_cancelled_impl() { std::terminate(); }

    // send
    int handle_send_completion()
    {
      return static_cast<Derived*>(this)->handle_send_completion_impl();
    }
    int handle_send_completion_impl() { return 0; }

    // tagged send
    int handle_tagged_send_completion(void* user_data)
    {
      return static_cast<Derived*>(this)->handle_tagged_send_completion_impl(user_data);
    }
    int handle_tagged_send_completion_impl(void* /*user_data*/) { return 0; }

    // recv
    int handle_recv_completion(uint64_t len)
    {
      return static_cast<Derived*>(this)->handle_recv_completion_impl(len);
    }
    int handle_recv_completion_impl(uint64_t /*len*/) { return 0; }

    // tagged recv
    int handle_tagged_recv_completion(void* user_data)
    {
      return static_cast<Derived*>(this)->handle_tagged_recv_completion_impl(user_data);
    }
    int handle_tagged_recv_completion_impl(bool /*threadlocal*/) { return 0; }

    int handle_rma_read_completion()
    {
      return static_cast<Derived*>(this)->handle_rma_read_completion_impl();
    }
    int handle_rma_read_completion_impl() { return 0; }

    // unknown sender = new connection (not needed when using pmi/mpi/other connection setup)
    template <typename Controller>
    int handle_new_connection(Controller* ctrl, uint64_t len)
    {
      return static_cast<Derived*>(this)->handle_new_connection_impl(ctrl, len);
    }

    template <typename Controller>
    int handle_new_connection_impl(Controller*, uint64_t)
    {
      return 0;
    }
  };

  // provided so that a pointer can be cast to this and the operation_context_type queried
  struct unspecialized_context : public operation_context_base<unspecialized_context>
  {
  };
}    // namespace libfatbat
