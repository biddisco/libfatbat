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

#include <iostream>
#include <unistd.h>
#include <utility>
//
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
//
#include "libfatbat_defines.hpp"
//
#include "libfatbat/fabric_error.hpp"
#include "libfatbat/logging.hpp"

// ------------------------------------------------------------------
#if (FI_MAJOR_VERSION < 2)
static_assert(false, "libfatbat requires libfabric version 2.0 or higher");
#endif
// ------------------------------------------------------------------

namespace libfatbat {

  MAKE_LOGGER(memrgn_log, "Region")

  // --------------------------------------------------------------------
  // region provider : just an abstraction around actual calls to libfabric
  // it exists due to the way oomph allowed multiple memory providers to be used
  // --------------------------------------------------------------------
  struct region_provider
  {
    // The internal memory region handle
    using provider_region = struct fid_mr;
    using provider_domain = struct fid_domain;

    // register region
    static inline int fi_register_memory(provider_domain* pd, int device_id, void const* buf,
        size_t len, uint64_t access_flags, uint64_t offset, uint64_t request_key,
        struct fid_mr** mr)
    {
      LIBFATBAT_SCOPE(memrgn_log, "{:<20} {} {:#10x} {:05}", __func__, buf, len, device_id);
      //
      struct iovec addresses = {/*.iov_base = */ const_cast<void*>(buf), /*.iov_len = */ len};
      fi_mr_attr attr = {
          /*.mr_iov         = */ {&addresses},
          /*.iov_count      = */ 1,
          /*.access         = */ access_flags,
          /*.offset         = */ offset,
          /*.requested_key  = */ request_key,
          /*.context        = */ nullptr,
          /*.auth_key_size  = */ 0,
          /*.auth_key       = */ nullptr,
          /*.iface          = */ FI_HMEM_SYSTEM,
          /*.device         = */ {0},
          /*.hmem_data      = */ nullptr,
          /*page_size       = */ static_cast<size_t>(sysconf(_SC_PAGESIZE)),
          /*base_mr         = */ nullptr,
          /*sub_mr_cnt      = */ 0,
      };

      if (device_id >= 0)
      {
#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
        LIBFATBAT_SCOPE(memrgn_log, "{:<20} {} {:#10x} {:05}", "device memory", buf, len, device_id);
        attr.device.cuda = device_id;
        int handle = device_id;    // hwmalloc::get_device_id();
//        attr.device.cuda = handle;
# if defined(LIBFATBAT_ENABLE_CUDA)
        attr.iface = FI_HMEM_CUDA;
        LIBFATBAT_TRACE(memrgn_log, "CUDA set device id {} {}", device_id, handle);
# elif defined(LIBFATBAT_ENABLE_HIP)
        attr.iface = FI_HMEM_ROCR;
        LIBFATBAT_TRACE(memrgn_log, "HIP set device id {} {}", device_id, handle);
# endif
#endif
      }
      uint64_t flags = 0;
      int ret = fi_mr_regattr(pd, &attr, flags, mr);
      if (ret) { throw libfatbat::fabric_error(int(ret), "register_memory"); }
      return ret;
    }

    // unregister region
    static inline int unregister_memory(provider_region* region) { return fi_close(&region->fid); }

    // Default registration flags for this provider
    static inline constexpr int access_flags()
    {
      return FI_READ | FI_WRITE | FI_RECV | FI_SEND | FI_REMOTE_READ | FI_REMOTE_WRITE;
    }

    // Get the local descriptor of the memory region.
    static inline void* get_local_key(provider_region* const region) { return fi_mr_desc(region); }

    // Get the remote key of the memory region.
    static inline uint64_t get_remote_key(provider_region* const region)
    {
      return fi_mr_key(region);
    }
  };

  // --------------------------------------------------------------------
  // This is a handle to a small chunk of memory that has been registered
  // as part of a much larger allocation (a memory_segment)
  struct memory_region
  {
    //
    using provider_domain = region_provider::provider_domain;
    using provider_region = region_provider::provider_region;

    // explicitly default these to ensure the compiler treats them as "trivial"
    memory_region() = default;
    memory_region(memory_region const&) = default;
    memory_region(memory_region&&) = default;
    memory_region& operator=(memory_region const&) = default;
    memory_region& operator=(memory_region&&) = default;

    memory_region(provider_region* region, unsigned char* addr, std::size_t size) noexcept
      : region_{region}
      , address_{addr}
      , size_{uint32_t(size)}
    {
      LIBFATBAT_SCOPE(memrgn_log, "{:<20} {} {:#10x} {:05}", __func__, (void*) addr, size, -1);
    }

    // --------------------------------------------------------------------
    // Return the address of this memory region block.
    inline unsigned char* get_address(void) const { return address_; }

    // --------------------------------------------------------------------
    // Get the local descriptor of the memory region.
    inline void* get_local_key(void) const { return region_provider::get_local_key(region_); }

    // --------------------------------------------------------------------
    // Get the remote key of the memory region.
    inline uint64_t get_remote_key(void) const { return region_provider::get_remote_key(region_); }

    // --------------------------------------------------------------------
    // Get the size of the memory chunk usable by this memory region,
    // this may be smaller than the value returned by get_length
    // if the region is a sub region (partial region) within another block
    inline uint64_t get_size(void) const { return size_; }

    // --------------------------------------------------------------------
    inline void release_region() noexcept { region_ = nullptr; }

    // --------------------------------------------------------------------
    // return the underlying libfabric region handle
    inline provider_region* get_region() const { return region_; }

    // --------------------------------------------------------------------
    // Deregister (unpin) memory region: returns 0 when successful, -1 otherwise
    inline int deregister(void) const;

    // --------------------------------------------------------------------
    friend std::ostream& operator<<(std::ostream& os, memory_region const& region)
    {
      (void) region;
#ifdef LIBFATBAT_LOGGING_ENABLED
      os << fmt::format(
          "region {:p} fi_region {:p} address {:p} size {:#06x} loc key {} rem key {}",
          (void*) (&region), (void*) (region.region_), (void*) (region.address_), region.size_,
          region.region_ ? region_provider::get_local_key(region.region_) : nullptr,
          region.region_ ? region_provider::get_remote_key(region.region_) : 0);
#endif
      return os;
    }

protected:
    // The hardware level handle to the region (as returned from libfabric fi_mr_reg)
    mutable provider_region* region_;

    // This gives the start address of this region.
    // This is the address that should be used for data storage
    unsigned char* address_;

    // The (maximum available) size of the memory buffer
    uint32_t size_;
  };

  // --------------------------------------------------------------------
  static_assert(std::is_trivially_copyable_v<memory_region>,
      "memory_region must be trivially copyable for serialization");
  // --------------------------------------------------------------------

}    // namespace libfatbat

#ifdef LIBFATBAT_LOGGING_ENABLED
template <>
struct fmt::formatter<libfatbat::memory_region> : fmt::ostream_formatter
{
};
#endif

// --------------------------------------------------------------------
// This is declared after the fmt::formatter specialization to avoid use
// before instantiation since it calls the fmt::formatter specialization
inline int libfatbat::memory_region::deregister(void) const
{
  if (region_)    //&& !get_user_region())
  {
    LIBFATBAT_TRACE(memrgn_log, "{:<20} {} ", "release", (void*) region_);
    //
    if (region_provider::unregister_memory(region_))
    {
      LIBFATBAT_ERROR(memrgn_log, "{:<20} mr failed {} ", "fi_close", *this);
      return -1;
    }
    else
    {
      LIBFATBAT_TRACE(memrgn_log, "{:<20} {}", "de-Registered", *this);
    }
    region_ = nullptr;
  }
  return 0;
}
