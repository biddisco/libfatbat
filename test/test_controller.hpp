#pragma once

#include "libfatbat/controller_base.hpp"
#include "libfatbat/locality.hpp"
#include "libfatbat/logging.hpp"
//
#include "pmi_helper.hpp"

// --------------------------------------------------------------------
class test_controller : public libfatbat::controller_base<test_controller>
{
  int rank = -1;
  int node = -1;
  int size = -1;

  public:
  // --------------------------------------------------------------------
  void initialize_derived(std::string const& provider, bool rootnode, int size, std::size_t threads)
  {
  }

  // --------------------------------------------------------------------
  constexpr fi_threading threadlevel_flags()
  {
#if defined(HAVE_LIBFATBAT_GNI) || defined(HAVE_LIBFATBAT_LNX)
    return FI_THREAD_ENDPOINT;
#else
    return FI_THREAD_SAFE;
#endif
  }

  // --------------------------------------------------------------------
  uint64_t caps_flags(uint64_t /*available_flags*/) const
  {
    uint64_t flags_required = FI_TAGGED;
#ifndef HAVE_LIBFATBAT_LNX
    flags_required |= FI_MSG | FI_TAGGED | FI_RECV | FI_SEND | FI_RMA | FI_READ | FI_WRITE |
        FI_REMOTE_READ | FI_REMOTE_WRITE;
# if LIBFATBAT_ENABLE_DEVICE
    flags_required |= FI_HMEM;
# endif
#endif
    return flags_required;
  }

  // --------------------------------------------------------------------
  // we do not need to perform any special actions on init (to init address)
  struct fi_info* set_src_dst_addresses(struct fi_info* info, bool tx)
  {    //
    return fi_dupinfo(info);
  }

  void boot_PMI() {}
};
