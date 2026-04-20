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

#include <string>

#if defined(LIBFATBAT_LOGGING_ENABLED)

# include <algorithm>
# include <cstddef>
# include <cstdint>
# include <cstring>
# include <string>
# include <type_traits>
//
# if !defined(SPDLOG_ACTIVE_LEVEL)
#  error "SPDLOG_ACTIVE_LEVEL is not defined. Rerun cmake."
# endif

# include <spdlog/cfg/env.h>
# include <spdlog/fmt/ostr.h>
# include <spdlog/sinks/stdout_color_sinks.h>
# include <spdlog/spdlog.h>
//
# include <boost/crc.hpp>
# include <fmt/args.h>
# include <fmt/format.h>
# include <fmt/ostream.h>
# include <fmt/ranges.h>

// ---------------------------------------------------------------------------
// Logging macros that use a named logger.
//
// These wrap spdlog's SPDLOG_LOGGER_* macros so that compile-time filtering
// via SPDLOG_ACTIVE_LEVEL works correctly. The first argument is always a
// std::shared_ptr<spdlog::logger>.
// ---------------------------------------------------------------------------
# define LIBFATBAT_TRACE(logger, ...) SPDLOG_LOGGER_TRACE(logger, __VA_ARGS__)
# define LIBFATBAT_DEBUG(logger, ...) SPDLOG_LOGGER_DEBUG(logger, __VA_ARGS__)
# define LIBFATBAT_INFO(logger, ...) SPDLOG_LOGGER_INFO(logger, __VA_ARGS__)
# define LIBFATBAT_WARN(logger, ...) SPDLOG_LOGGER_WARN(logger, __VA_ARGS__)
# define LIBFATBAT_ERROR(logger, ...) SPDLOG_LOGGER_ERROR(logger, __VA_ARGS__)
# define LIBFATBAT_CRITICAL(logger, ...) SPDLOG_LOGGER_CRITICAL(logger, __VA_ARGS__)

namespace libfatbat::log {

  // -------------------------------------------------------------------------
  /// Helper for creating a format string for scoped messages
  // -------------------------------------------------------------------------
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

  struct scoped_var
  {
    std::string const message_;
    std::shared_ptr<spdlog::logger> logger_;

    template <typename... Args>
    scoped_var(std::shared_ptr<spdlog::logger> logger, std::string format, Args const&... args)
      : message_(make_scope_message(format, args...))
      , logger_(std::move(logger))
    {
      LIBFATBAT_DEBUG(logger_, "{:<20} {}", ">> enter <<", message_);
    }

    ~scoped_var() { LIBFATBAT_DEBUG(logger_, "{:<20} {}", "<< leave >>", message_); }
  };

  // -------------------------------------------------------------------------
  // Create (or retrieve) a named logger. Thread-safe. If the logger already
  // exists in spdlog's registry it is returned; otherwise a new
  // stderr-colour logger is created and registered.
  // -------------------------------------------------------------------------
  using log_type = std::shared_ptr<spdlog::logger>;
  inline log_type create(std::string const& name)
  {
    auto logger = spdlog::get(name);
    if (!logger)
    {
      logger = spdlog::stderr_color_mt(name);
      // Inherit the global level that was set during initialisation
      logger->set_level(spdlog::get_level());
    }
    return logger;
  }
# define MAKE_LOGGER(name, text)                                                                   \
   inline libfatbat::log::log_type name = libfatbat::log::create(#text);

  // ------------------------------------------------------------------
  // helper class for printing short memory dump and crc32
  // useful for debugging corruptions in buffers during message transfers
  // ------------------------------------------------------------------
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
};    // namespace libfatbat::log

// ------------------------------------------------------------------
// Formatter for mem_crc32 that prints a hex dump and crc32 of a memory region
// ------------------------------------------------------------------
template <>
struct fmt::formatter<libfatbat::log::mem_crc32>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(libfatbat::log ::mem_crc32 const& value, FormatContext& ctx) const
  {
    auto out = ctx.out();
    out = fmt::format_to(out, "Memory: address {} length {:06x} CRC32:{:08x}\n",
        fmt::ptr(value.addr_), value.len_, libfatbat::log ::crc32(value.addr_, value.len_));

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

// ------------------------------------------------------------------
// Helper macro to log entry and exit of a scope.
// ------------------------------------------------------------------
# define LIBFATBAT_SCOPE(logger, format, ...)                                                      \
   libfatbat::log::scoped_var local_scoped_var(logger, format, __VA_ARGS__);
// ----------------------------------------------------------------------------

namespace libfatbat::log {

  // -------------------------------------------------------------------------
  /// Call once at application start-up (e.g. in main()) to configure the
  /// global log pattern and level from environment variables.
  ///
  /// Recognised variables:
  ///   LIBFATBAT_PATTERN  /  LIBFATBAT_PATTERN   - log line pattern
  ///   LIBFATBAT_LEVEL    /  LIBFATBAT_LEVEL     - global level + per-logger overrides
  ///
  /// If none are set a sensible default pattern is used.
  inline void init_from_env()
  {
    // --- pattern ---
    char const* pattern = std::getenv("LIBFATBAT_PATTERN");
    if (!pattern || pattern[0] == '\0') pattern = std::getenv("LIBFATBAT_PATTERN");

    if (pattern && pattern[0] != '\0')
      spdlog::set_pattern(pattern);
    else
      spdlog::set_pattern("[%^%-8l%$] %t |%=10!n| %v");

    // set log level from thee user specified compilation level
    spdlog::set_level(static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL));
    // reads LIBFATBAT_LEVEL for per-logger overrides (e.g. "mylogger=debug,other=info") and applies them.
    spdlog::cfg::load_env_levels();

    // We additionally support LIBFATBAT_LEVEL as an override.
    char const* level = std::getenv("LIBFATBAT_LEVEL");
    if (level && level[0] != '\0')
    {
      if (static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL) >
          spdlog::level::from_str(level))
      {
        // If the user-specified level is more verbose than the compile-time level, log a warning.
        spdlog::log(spdlog::level::off,
            "LIBFATBAT_LEVEL={} is more verbose than the compile-time level '{}', "
            "but will be applied anyway. (Recompile with a lower SPDLOG_ACTIVE_LEVEL).",
            level,
            spdlog::level::to_string_view(
                static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL)));
      }
      if (static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL) <
          spdlog::level::from_str(level))
      {
        // If the user-specified level is more verbose than the compile-time level, log a warning.
        spdlog::log(spdlog::level::off,
            "LIBFATBAT_LEVEL={} is less verbose than the compile-time level '{}'", level,
            spdlog::level::to_string_view(
                static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL)));
      }
      spdlog::set_level(spdlog::level::from_str(level));
    }
  }
}    // namespace libfatbat::log

#else

// In increasing level
# define LIBFATBAT_TRACE(...)
# define LIBFATBAT_DEBUG(...)
# define LIBFATBAT_INFO(...)
# define LIBFATBAT_WARN(...)
# define LIBFATBAT_ERROR(...)
# define LIBFATBAT_CRITICAL(...)
//
# define LIBFATBAT_SCOPE(...)
//
# define MAKE_LOGGER(name, text)
namespace libfatbat::log {
  inline void init_from_env() {}
}    // namespace libfatbat::log

#endif
