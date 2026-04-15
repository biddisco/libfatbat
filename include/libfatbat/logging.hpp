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
# include <string>
# include <type_traits>
//
# include <boost/crc.hpp>
# include <fmt/args.h>
# include <fmt/format.h>
# include <fmt/ostream.h>
# include <fmt/ranges.h>
# include <spdlog/fmt/ostr.h>
# include <spdlog/spdlog.h>

namespace libfatbat::logging {

  template <typename T>
  inline constexpr bool is_format_fragment_v =
      std::is_convertible_v<std::decay_t<T>, fmt::string_view>;

  template <typename... Args>
  inline std::string make_scope_message(fmt::string_view format, Args const&... args)
  {
    std::string effective_format(format.data(), format.size());
    std::size_t num_fields = sizeof...(args);
    std::size_t num_data_args = 0;
    fmt::dynamic_format_arg_store<fmt::format_context> store;

    auto process_arg = [&](auto const& arg) {
      using arg_type = std::decay_t<decltype(arg)>;
      if (num_data_args < num_fields)
      {
        store.push_back(arg);
        ++num_data_args;
        return;
      }

      if constexpr (is_format_fragment_v<arg_type>)
      {
        fmt::string_view const fragment(arg);
        effective_format += " ";
        effective_format.append(fragment.data(), fragment.size());
        num_fields += sizeof...(args) - num_data_args;
      }
      else
      {
        effective_format += " {}";
        store.push_back(arg);
        ++num_fields;
        ++num_data_args;
      }
    };

    (process_arg(args), ...);
    return fmt::vformat(effective_format, store);
  }
}    // namespace libfatbat::logging

template <typename... Args>
struct scoped_var
{
  std::string const message_;
  //
  explicit scoped_var(std::string format, Args const&... args)
    : message_(libfatbat::logging::make_scope_message(format, args...))
  {
    SPDLOG_TRACE("{:20} {}", ">> enter <<", message_);
  }

  ~scoped_var() { SPDLOG_TRACE("{:20} {}", "<< leave >>", message_); }
};
# define SPDLOG_SCOPE(format, ...) scoped_var local_scoped_var(format, __VA_ARGS__);

// ------------------------------------------------------------------
// helper class for printing short memory dump and crc32
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
