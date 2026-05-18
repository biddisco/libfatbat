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

  enum class mem_Iface
  {
    System,
    Cuda,
    Rocr
  };

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
        LIBFATBAT_SCOPE(
            memrgn_log, "{:<20} {} {:#10x} {:05}", "device memory", buf, len, device_id);
        attr.device.cuda = device_id;
        int handle = device_id;
# if defined(LIBFATBAT_HAVE_CUDA)
        attr.iface = FI_HMEM_CUDA;
        LIBFATBAT_TRACE(memrgn_log, "CUDA set device id {} {}", device_id, handle);
# elif defined(LIBFATBAT_HAVE_HIP)
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
  struct region_info
  {
    uint64_t addr = 0;
    uint64_t rkey = 0;
    uint64_t size = 0;
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

    region_info get_info() const
    {
      region_info info;
      info.addr = get_addr();
      info.rkey = get_remote_key();
      info.size = get_size();
      return info;
    }

    // --------------------------------------------------------------------
    // Return the address of this memory region block.
    inline unsigned char* get_address(void) const { return address_; }
    inline std::uintptr_t get_addr(void) const
    {
      return reinterpret_cast<std::uintptr_t>(address_);
    }

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
    // register the memory
    inline void register_memory(provider_domain* pd, bool bind_mr, void* ep, int device_id);

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

namespace libfatbat {

  // --------------------------------------------------------------------
  // registering an address buffer
  inline void memory_region::register_memory(
      provider_domain* pd, bool bind_mr, void* ep, int device_id)
  {
    // an rma key counter to keep some providers (CXI) happy
    static std::atomic<std::uint64_t> key = 0;
    //
    int ret = region_provider::fi_register_memory(
        pd, device_id, address_, size_, region_provider::access_flags(), 0, key++, &(region_));
    if (!ret)
    {
      LIBFATBAT_TRACE(memrgn_log, "{:<20} {} {}", "Registered region", device_id, (void*) this);
    }

    if (bind_mr)
    {
      ret = fi_mr_bind(region_, (struct fid*) ep, 0);
      if (ret) { throw fabric_error(int(ret), "fi_mr_bind"); }
      else
      {
        LIBFATBAT_TRACE(memrgn_log, "Bound region {}", (void*) this);
      }

      ret = fi_mr_enable(region_);
      if (ret) { throw fabric_error(int(ret), "fi_mr_enable"); }
      else
      {
        LIBFATBAT_TRACE(memrgn_log, "Enabled region {}", (void*) this);
      }
    }
  }

  // --------------------------------------------------------------------
  // This is declared after the fmt::formatter specialization to avoid use
  // before instantiation since it calls the fmt::formatter specialization
  inline int memory_region::deregister(void) const
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

  // --------------------------------------------------------------------
  // construct a memory region object by registering an existing address buffer
  // --------------------------------------------------------------------
  inline memory_region make_region(region_provider::provider_domain* pd, void const* buffer,
      uint64_t const length, bool bind_mr, void* ep, int device_id)
  {
    memory_region mem_r{nullptr, reinterpret_cast<unsigned char*>((void*) buffer), length};
    mem_r.register_memory(pd, bind_mr, ep, device_id);
    return mem_r;
  }

  template <typename Controller>
  inline memory_region
  make_region(Controller* controller, void* const ptr, std::size_t size, int device_id)
  {
    if (controller->get_mrbind())
    {
      void* endpoint = controller->get_tx_endpoint().get_ep();
      auto temp_region =
          make_region(controller->get_domain(), ptr, size, true, endpoint, device_id);
      return temp_region;
    }
    else
    {
      auto temp_region =
          make_region(controller->get_domain(), ptr, size, false, nullptr, device_id);
      return temp_region;
    }
  }
}    // namespace libfatbat
