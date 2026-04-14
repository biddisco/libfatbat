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

#include <array>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <utility>
//
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
//
#include <arpa/inet.h>
#include <netinet/in.h>
//
#include "libfatbat/logging.hpp"
#include "libfatbat_defines.hpp"

// Different providers use different address formats that we accommodate in our locality object.
#ifdef HAVE_LIBFATBAT_GNI
# define HAVE_LIBFATBAT_LOCALITY_SIZE 48
#endif

#ifdef HAVE_LIBFATBAT_CXI
# ifdef HAVE_LIBFATBAT_CXI_1_15
#  define HAVE_LIBFATBAT_LOCALITY_SIZE sizeof(int)
# else
#  define HAVE_LIBFATBAT_LOCALITY_SIZE sizeof(long int)
# endif
#endif

#ifdef HAVE_LIBFATBAT_EFA
# define HAVE_LIBFATBAT_LOCALITY_SIZE 32
#endif

#if defined(HAVE_LIBFATBAT_VERBS) || defined(HAVE_LIBFATBAT_TCP) ||                                \
    defined(HAVE_LIBFATBAT_SOCKETS) || defined(HAVE_LIBFATBAT_PSM2)
# define HAVE_LIBFATBAT_LOCALITY_SIZE 16
#endif

#if defined(HAVE_LIBFATBAT_SHM)
# define HAVE_LIBFATBAT_LOCALITY_SIZE 24
#endif

#if defined(HAVE_LIBFATBAT_LNX)
# define HAVE_LIBFATBAT_LOCALITY_SIZE 32
#endif

namespace libfatbat {

  struct locality;

  // --------------------------------------------------------------------
  // All the tcp stuff is obsolete as we use PMI or MPI for bootstrapping
  // in all current applications, all this code really needs is a buffer
  // and some get/set features.
  // Old comments left in for reference.
  // --------------------------------------------------------------------

  // --------------------------------------------------------------------
  // Locality, in this structure we store the information required by
  // libfabric to make a connection to another node.
  // With libfabric 1.4.x the array contains the fabric ip address stored
  // as the second uint32_t in the array. For this reason we use an
  // array of uint32_t rather than uint8_t/char so we can easily access
  // the ip for debug/validation purposes
  // --------------------------------------------------------------------
  namespace locality_defs {
    // the number of 32bit ints stored in our array
    uint32_t const array_size = HAVE_LIBFATBAT_LOCALITY_SIZE;
    uint32_t const array_length = HAVE_LIBFATBAT_LOCALITY_SIZE / 4;
  }    // namespace locality_defs

  struct locality
  {
    // array type of our locality data
    typedef std::array<uint32_t, locality_defs::array_length> locality_data;

    static char const* type() { return "libfabric"; }

    explicit locality(locality_data const& in_data, struct fid_av* av)
    {
      std::memcpy(&data_[0], &in_data[0], locality_defs::array_size);
      fi_address_ = 0;
      av_ = av;
      SPDLOG_TRACE("{:20} {}", "explicit construct", to_str());
    }

    locality()
    {
      std::memset(&data_[0], 0x00, locality_defs::array_size);
      fi_address_ = 0;
      av_ = nullptr;
      SPDLOG_TRACE("{:20} {}", "default construct", to_str());
    }

    locality(locality const& other)
      : data_(other.data_)
      , fi_address_(other.fi_address_)
      , av_(other.av_)
    {
      SPDLOG_TRACE("{:20} {}", "copy construct", to_str());
    }

    locality(locality const& other, fi_addr_t addr, struct fid_av* av)
      : data_(other.data_)
      , fi_address_(addr)
      , av_(av)
    {
      SPDLOG_TRACE("{:20} {}", "copy fi construct", to_str());
    }

    locality(locality&& other)
      : data_(std::move(other.data_))
      , fi_address_(other.fi_address_)
      , av_(other.av_)
    {
      SPDLOG_TRACE("{:20} {}", "move construct", to_str());
    }

    // provided to support sockets mode bootstrap
    explicit locality(std::string const& address, std::string const& portnum)
    {
      SPDLOG_TRACE("{:20} {}:{}", "explicit construct-2", address, portnum);
      //
      struct sockaddr_in socket_data;
      memset(&socket_data, 0, sizeof(socket_data));
      socket_data.sin_family = AF_INET;
      socket_data.sin_port = htons(std::stol(portnum));
      inet_pton(AF_INET, address.c_str(), &(socket_data.sin_addr));
      //
      std::memcpy(&data_[0], &socket_data, locality_defs::array_size);
      fi_address_ = 0;
      av_ = nullptr;
      SPDLOG_TRACE("{:20} {}", "string constructing", to_str());
    }

    locality& operator=(locality const& other)
    {
      data_ = other.data_;
      fi_address_ = other.fi_address_;
      av_ = other.av_;
      SPDLOG_TRACE("{:20} {} {}", "copy operator", to_str(), other.to_str());
      return *this;
    }

    bool operator==(locality const& other)
    {
      SPDLOG_TRACE("{:20} {} {}", "equality operator", to_str(), other.to_str());
      return std::memcmp(&data_, &other.data_, locality_defs::array_size) == 0;
    }

    inline fi_addr_t const& fi_address() const { return fi_address_; }

    inline void set_fi_address(fi_addr_t fi_addr) { fi_address_ = fi_addr; }

    inline uint16_t port() const
    {
      uint16_t port = 256 * reinterpret_cast<uint8_t const*>(data_.data())[2] +
          reinterpret_cast<uint8_t const*>(data_.data())[3];
      return port;
    }

    inline locality_data const& fabric_data() const { return data_; }

    inline char* fabric_data_writable() { return reinterpret_cast<char*>(data_.data()); }

    std::string to_str(struct fid_av* av = nullptr) const
    {
      char sbuf[256];
      size_t buflen = 256;
      if (!av_ && !av) { return "No address vector"; }
      char const* straddr_ret = fi_av_straddr(av_ ? av_ : av, data_.data(), sbuf, &buflen);
      std::string result = straddr_ret ? straddr_ret : "Address formatting Error";
      return result;
    }

private:
    friend bool operator==(locality const& lhs, locality const& rhs)
    {
      SPDLOG_TRACE("{:20} {} {}", "equality friend", lhs.to_str(), rhs.to_str());
      return ((lhs.data_ == rhs.data_) && (lhs.fi_address_ == rhs.fi_address_));
    }

    friend std::ostream& operator<<(std::ostream& os, locality const& loc)
    {
      for (uint32_t i = 0; i < locality_defs::array_length; ++i) { os << loc.data_[i]; }
      return os;
    }

private:
    locality_data data_;
    fi_addr_t fi_address_;
    struct fid_av* av_;
  };

}    // namespace libfatbat
