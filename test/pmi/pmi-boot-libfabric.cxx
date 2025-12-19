/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "../test_controller.hpp"
#include "libfatbat/logging.hpp"

// ----------------------------------------------------------------------------
int main(int argx, char** argv)
{
#if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::trace);
#endif

  std::size_t rank;
  std::size_t size;
  std::size_t nthreads;
  test_controller controller;
  pmi_helper pmi;

  std::tie(rank, size) = pmi.init_PMI();
  controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, nthreads);
  pmi.boot_PMI(&controller);
  pmi.finalize_PMI();
  {
    SPDLOG_SCOPE("{}", "debug AV");
    controller.debug_print_av_vector(size);
  }
}
