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
#include "libfatbat/fabric_error.hpp"
#include "libfatbat/logging.hpp"
#include "libfatbat/memory_region.hpp"

// ------------------------------------------------------------------
#if (FI_MAJOR_VERSION < 2)
static_assert(false, "libfatbat requires libfabric version 2.0 or higher");
#endif
// ------------------------------------------------------------------

namespace libfatbat {

  // --------------------------------------------------------------------
  // Same as memory_region but also tracks the size of the message stored in the region
  // which allows a larger region to be used for smaller messages and provides a "used space"
  struct usedspace_memory_region_ : public memory_region
  {
    // Space used by a message in the memory region.
    // This may be smaller/less than the size available if more space
    // was allocated than it turns out was needed
    mutable uint32_t used_space_;

    //
    usedspace_memory_region_() noexcept
      : memory_region()
      , used_space_{0}
    {
    }

    usedspace_memory_region_(usedspace_memory_region_ const& handle) noexcept
      : memory_region(handle)
      , used_space_{handle.used_space_}
    {
    }

    usedspace_memory_region_(provider_region* region, unsigned char* address, uint64_t size,
        uint32_t used_space) noexcept
      : memory_region{region, address, size}
      , used_space_{used_space}
    {
    }

    // --------------------------------------------------------------------
    // Get the size used by a message in the memory region.
    inline uint32_t get_used_space(void) const { return used_space_; }

    // --------------------------------------------------------------------
    // Set the size used by a message in the memory region.
    inline void set_used_space(uint32_t length) { used_space_ = length; }
  };

}    // namespace libfatbat

namespace libfatbat {
  // --------------------------------------------------------------------
  // a memory segment is a pinned block of memory that has been specialized
  // by a particular region provider. Each provider (infiniband, libfabric,
  // other) has a different definition for the object and the protection
  // domain used to limit access.
  // --------------------------------------------------------------------
  struct memory_segment : public memory_region
  {
    using memory_region::provider_domain;
    using memory_region::provider_region;
    using handle_type = memory_region;

    // --------------------------------------------------------------------
    memory_segment(
        provider_region* region, unsigned char* address, unsigned char* base_address, uint64_t size)
      : memory_region{region, address, size}
      , base_addr_(base_address)
    {
    }

    // --------------------------------------------------------------------
    // move constructor, clear other region
    memory_segment(memory_segment&& other) noexcept
    {
      // Copy the data members manually
      this->address_ = other.address_;
      this->size_ = other.size_;

      // this makes it non-trivial, we do not want to deregister the region twice
      this->region_ = std::exchange(other.region_, nullptr);
      // this one is not strictly necessary, but for sanity
      this->base_addr_ = std::exchange(other.base_addr_, nullptr);
    }

    // --------------------------------------------------------------------
    // move assignment, clear other region
    memory_segment& operator=(memory_segment&& other) noexcept
    {
      if (this != &other)
      {
        this->address_ = other.address_;
        this->size_ = other.size_;
        // this makes it non-trivial, we do not want to deregister the region twice
        this->region_ = std::exchange(other.region_, nullptr);
        // this one is not strictly necessary, but for sanity
        this->base_addr_ = std::exchange(other.base_addr_, nullptr);
      }
      return *this;
    }
    // --------------------------------------------------------------------
    // construct a memory region object by registering an existing address buffer
    // we do not cache local/remote keys here because memory segments are only
    // used by the heap to store chunks and the user will always receive
    // a memory_region - which does have keys cached
    memory_segment(provider_domain* pd, void const* buffer, uint64_t const length, bool bind_mr,
        void* ep, int device_id)
    {
      // an rma key counter to keep some providers (CXI) happy
      static std::atomic<std::uint64_t> key = 0;
      //
      address_ = static_cast<unsigned char*>(const_cast<void*>(buffer));
      size_ = length;
      region_ = nullptr;
      //
      base_addr_ = memory_region::address_;
      LIBFATBAT_TRACE(memrgn_log, "{:<20} {} {}", "memory_segment", (void*) this, device_id);

      int ret = region_provider::fi_register_memory(
          pd, device_id, buffer, length, region_provider::access_flags(), 0, key++, &(region_));
      if (!ret)
      {
        LIBFATBAT_TRACE(memrgn_log, "{:<20} {} {}", "Registered region", device_id, (void*) this);
      }

      if (bind_mr)
      {
        ret = fi_mr_bind(region_, (struct fid*) ep, 0);
        if (ret) { throw libfatbat::fabric_error(int(ret), "fi_mr_bind"); }
        else
        {
          LIBFATBAT_TRACE(memrgn_log, "Bound region {}", (void*) this);
        }

        ret = fi_mr_enable(region_);
        if (ret) { throw libfatbat::fabric_error(int(ret), "fi_mr_enable"); }
        else
        {
          LIBFATBAT_TRACE(memrgn_log, "Enabled region {}", (void*) this);
        }
      }
    }

    // --------------------------------------------------------------------
    // destroy the region and memory according to flag settings
    ~memory_segment()
    {    //
      deregister();
    }

    // --------------------------------------------------------------------
    handle_type get_handle(std::size_t offset, std::size_t size) const noexcept
    {
      return memory_region(region_, base_addr_ + offset, size);
    }

    // --------------------------------------------------------------------
    // Get the address of the base memory region.
    // This is the address of the memory allocated from the system
    inline unsigned char* get_base_address(void) const { return base_addr_; }

    // --------------------------------------------------------------------
    friend std::ostream& operator<<(std::ostream& os, memory_segment const& region)
    {
      (void) region;
#ifdef LIBFATBAT_LOGGING_ENABLED
      os << *static_cast<memory_region const*>(&region)
         << fmt::format("base_addr {}", (void*) (region.base_addr_));
#endif
      return os;
    }

public:
    // this is the base address of the memory registered by this segment
    // individual memory_regions are offset from this address
    unsigned char* base_addr_;
  };
}    // namespace libfatbat
