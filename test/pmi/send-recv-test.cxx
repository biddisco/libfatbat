/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <iostream>
//
#include <boost/program_options.hpp>
#include <hwmalloc/heap.hpp>
//
#include "../test_controller.hpp"
#include "libfatbat/logging.hpp"

struct context
{
  using region_type = libfatbat::memory_segment;
  using domain_type = region_type::provider_domain;
  using device_region_type = libfatbat::memory_segment;
  using heap_type = hwmalloc::heap<context>;
  // using callback_queue = boost::lockfree::queue<detail::shared_request_state*,
  //     boost::lockfree::fixed_sized<false>, boost::lockfree::allocator<std::allocator<void>>>;

  test_controller* m_controller;
  domain_type* m_domain;

  context(test_controller* controller)
    : m_controller(controller)
    , m_domain(m_controller->get_domain())
  {
    std::cout << "context constructor" << std::endl;
  }

  ~context() { std::cout << "context destructor" << std::endl; }

  region_type make_region(void* const ptr, std::size_t size, int device_id)
  {
    if (m_controller->get_mrbind())
    {
      void* endpoint = m_controller->get_rx_endpoint().get_ep();
      return region_type(m_domain, ptr, size, true, endpoint, device_id);
    }
    else
    {
      return region_type(m_domain, ptr, size, false, nullptr, device_id);
    }
  }
};

// auto register_memory(context&, void* ptr, std::size_t) { return context::region{ptr}; }
// --------------------------------------------------------------------
inline context::region_type register_memory(context& c, void* const ptr, std::size_t size)
{
  return c.make_region(ptr, size, -2);
}

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

  using heap_t = hwmalloc::heap<context>;
  context c(&controller);
  heap_t h(&c);

  std::vector<int> x;

  auto ptr = h.allocate(1024 * 1024 * 8, 0);
  SPDLOG_TRACE("{} {}", ptr.get(), libfatbat::debug::print_type<decltype(ptr)>());
  std::cout << ptr.get() << std::endl;
  // std::cout << ptr.device_ptr() << std::endl;
  h.free(ptr);

  return 0;
}
