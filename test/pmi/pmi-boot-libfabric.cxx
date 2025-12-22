/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <boost/program_options.hpp>
#include "../test_controller.hpp"
#include "libfatbat/logging.hpp"
// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
#if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::trace);
#endif

  namespace po = boost::program_options;
  po::options_description desc("Options");
  desc.add_options()("debug", "Enable debug mode");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  bool attach_debugger = vm.count("debug") > 0;

  std::size_t rank, size, nthreads = 2;    // anything >1 triggers thread safety code paths
  test_controller controller;
  pmi_helper pmi;

  std::tie(rank, size) = pmi.init_PMI(attach_debugger);
  controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, nthreads);
  pmi.boot_PMI(&controller);
  pmi.finalize_PMI();
  {
    SPDLOG_SCOPE("{}", "debug AV");
    controller.debug_print_av_vector(size);
  }
}
