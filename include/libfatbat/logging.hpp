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
  std::tuple<Args const&...> const message_;
  //
  explicit scoped_var(Args const&... args)
    : message_(args...)    //
  {
    SPDLOG_INFO("SCOPE >> enter << {}", message_);
  }

  ~scoped_var() { SPDLOG_INFO("SCOPE << leave >> {}", message_); }
};
# define SPDLOG_SCOPE(...) scoped_var scope(__VA_ARGS__);

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
