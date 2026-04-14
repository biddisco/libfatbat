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

#include <stdexcept>
#include <string>
//
#include <rdma/fi_errno.h>
//
#include "libfatbat/logging.hpp"

namespace libfatbat {

  class fabric_error : public std::runtime_error
  {
public:
    // --------------------------------------------------------------------
    fabric_error(int err, std::string const& msg)
      : std::runtime_error(std::string(fi_strerror(-err)) + msg)
      , error_(err)
    {
      SPDLOG_ERROR("{:20}: {}", msg, fi_strerror(-err));
      std::terminate();
    }

    fabric_error(int err)
      : std::runtime_error(fi_strerror(-err))
      , error_(-err)
    {
      SPDLOG_ERROR("{:20}: {}", "fabric_error", what());
      std::terminate();
    }

    int error_;
  };

}    // namespace libfatbat
