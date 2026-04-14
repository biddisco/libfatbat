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

#if defined(LIBFATBAT_LOGGING_ENABLED)

# include <algorithm>
# include <cstddef>
# include <cstdint>
# include <cstring>
# include <tuple>
//
# include <boost/crc.hpp>
# include <fmt/format.h>
# include <fmt/ostream.h>
# include <fmt/ranges.h>
# include <spdlog/fmt/ostr.h>
# include <spdlog/spdlog.h>

template <typename... Args>
struct scoped_var
{
  // capture tuple elements by reference - no temp vars in constructor please
  std::string format_;
  std::tuple<Args const&...> const message_;
  //
  explicit scoped_var(std::string format, Args const&... args)
    : format_(format)
    , message_(args...)    //
  {
    std::string message_str_ = std::apply(
        [&](auto const&... args) { return fmt::vformat(format_, fmt::make_format_args(args...)); },
        message_);
    SPDLOG_TRACE("{:20} {}", ">> enter <<", message_str_);
    // SPDLOG_INFO("{:20} {}", ">> enter <<", "message_str_");
    // SPDLOG_INFO("{:20} {}", ">> enter <<",
    //   fmt::vformat(format_, fmt::make_format_args(std::apply([](auto const&... args) { return std::make_tuple(args...); }, message_))));
  }

  ~scoped_var()
  {
    SPDLOG_TRACE("{:20} {}", "<< leave >>",
        std::apply(
            [&](auto const&... args) {
              return fmt::vformat(format_, fmt::make_format_args(args...));
            },
            message_));
  }
};
# define SPDLOG_SCOPE(format, ...) scoped_var local_scoped_var(format, __VA_ARGS__);

// ------------------------------------------------------------------
// helper function for printing short memory dump and crc32
// useful for debugging corruptions in buffers during message transfers
// ------------------------------------------------------------------
namespace libfatbat::logging {
  inline std::uint32_t crc32(void const* ptr, std::size_t size)
  {
    boost::crc_32_type result;
    result.process_bytes(ptr, size);
    return result.checksum();
  }

  struct mem_crc32
  {
    explicit mem_crc32(void const* a, std::size_t len, std::size_t wrap = 8)
      : addr_(reinterpret_cast<std::uint8_t const*>(a))
      , len_(len)
      , wrap_((std::max) (wrap, std::size_t(1)))
    {
    }

    std::uint8_t const* addr_;
    std::size_t const len_;
    std::size_t const wrap_;
  };
}    // namespace libfatbat::logging

template <>
struct fmt::formatter<libfatbat::logging::mem_crc32>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(libfatbat::logging::mem_crc32 const& value, FormatContext& ctx) const
  {
    auto out = ctx.out();
    out = fmt::format_to(out, "Memory: address {} length {:06x} CRC32:{:08x}\n",
        fmt::ptr(value.addr_), value.len_, libfatbat::logging::crc32(value.addr_, value.len_));

    std::size_t const num_chunks = (std::min) ((value.len_ + 7) / 8, std::size_t(128));
    for (std::size_t i = 0; i < num_chunks; ++i)
    {
      std::size_t const offset = i * 8;
      std::size_t const bytes = (std::min) (value.len_ - offset, std::size_t(8));
      std::uint64_t chunk = 0;
      std::memcpy(&chunk, value.addr_ + offset, bytes);
      out = fmt::format_to(out, "{:016x} ", chunk);
      if (i % value.wrap_ == (value.wrap_ - 1)) { out = fmt::format_to(out, "\n"); }
    }

    return out;
  }
};

#else

// In increasing level
# define SPDLOG_TRACE(...)
# define SPDLOG_DEBUG(...)
# define SPDLOG_INFO(...)
# define SPDLOG_WARN(...)
# define SPDLOG_ERROR(...)
# define SPDLOG_CRITICAL(...)
//
# define SPDLOG_SCOPE(...)

#endif
