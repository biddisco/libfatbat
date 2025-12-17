#pragma once

#if defined(FATBAT_LOGGING_ENABLED)

# include <tuple>
//
# include <fmt/ostream.h>
# include <fmt/ranges.h>

# include <spdlog/fmt/ostr.h>
# include <spdlog/spdlog.h>
//
# include "libfatbat/print_type.hpp"

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
    std::string message_str_ =
        fmt::vformat(format_, fmt::make_format_args(std::get<Args const&>(message_)...));
    SPDLOG_INFO("{:20} {}", ">> enter <<", message_str_);
    SPDLOG_INFO("{:20} {}", ">> enter <<", "message_str_");
    // SPDLOG_INFO("{:20} {}", ">> enter <<",
    //   fmt::vformat(format_, fmt::make_format_args(std::apply([](auto const&... args) { return std::make_tuple(args...); }, message_))));
  }

  ~scoped_var()
  {
    // SPDLOG_INFO("{:20} {}", "<< leave >>",
    // fmt::vformat(format_, fmt::make_format_args(std::apply([](auto const&... args) { return std::make_tuple(args...); }, message_))));
  }
};
# define SPDLOG_SCOPE(format, ...) scoped_var scope(format, __VA_ARGS__);

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
