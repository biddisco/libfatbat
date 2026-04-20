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

template <typename T>
class unique_function : public std::function<T>
{
  template <typename Fn, typename En = void>
  struct wrapper;

  // specialization for CopyConstructible Fn
  template <typename Fn>
  struct wrapper<Fn, std::enable_if_t<std::is_copy_constructible<Fn>::value>>
  {
    Fn fn;

    template <typename... Args>
    auto operator()(Args&&... args)
    {
      return fn(std::forward<Args>(args)...);
    }
  };

  // specialization for MoveConstructible-only Fn
  template <typename Fn>
  struct wrapper<Fn,
      std::enable_if_t<!std::is_copy_constructible<Fn>::value &&
          std::is_move_constructible<Fn>::value>>
  {
    Fn fn;

    wrapper(Fn&& fn)
      : fn(std::forward<Fn>(fn))
    {
    }

    wrapper(wrapper&&) = default;
    wrapper& operator=(wrapper&&) = default;

    // these two functions are instantiated by std::function
    // and are never called
    wrapper(wrapper const& rhs)
      : fn(const_cast<Fn&&>(rhs.fn))
    {
      throw 0;
    }    // hack to initialize fn for non-DefaultContructible types
    wrapper& operator=(wrapper&) { throw 0; }

    template <typename... Args>
    auto operator()(Args&&... args)
    {
      return fn(std::forward<Args>(args)...);
    }
  };

  using base = std::function<T>;

  public:
  unique_function() noexcept = default;
  unique_function(std::nullptr_t) noexcept
    : base(nullptr)
  {
  }

  template <typename Fn>
  unique_function(Fn&& f)
    : base(wrapper<Fn>{std::forward<Fn>(f)})
  {
  }

  unique_function(unique_function&&) = default;
  unique_function& operator=(unique_function&&) = default;

  unique_function& operator=(std::nullptr_t)
  {
    base::operator=(nullptr);
    return *this;
  }

  template <typename Fn>
  unique_function& operator=(Fn&& f)
  {
    base::operator=(wrapper<Fn>{std::forward<Fn>(f)});
    return *this;
  }

  using base::operator();
};

using rank_type = std::uint64_t;
using tag_type = std::uint64_t;
using request_callback_type = unique_function<void(rank_type, tag_type)>;

// ------------------------------------------------------------------
MAKE_LOGGER(ctxt_log, "Context")

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
