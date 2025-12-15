#ifndef LIBFATBAT_CONFIG_LIBFABRIC_HPP
#define LIBFATBAT_CONFIG_LIBFABRIC_HPP

// clang-format off
@namespace_config_defines@
// clang-format on

// ------------------------------------------------------------------
#define NS_DEBUG libfatbat::debug

#ifndef LF_DEB
# define LF_DEB(printer, Expr)                                                                     \
   {                                                                                               \
     using namespace NS_DEBUG;                                                                     \
     if constexpr (printer.is_enabled()) { printer.Expr; };                                        \
   }
#endif

#define LFSOURCE_DIR "@LIBFATBAT_SRC_DIR@"
#define LFPRINT_HPP "@LIBFATBAT_SRC_DIR@/print.hpp"
#define LFCOUNT_HPP "@LIBFATBAT_SRC_DIR@/simple_counter.hpp"

#if __has_include(LFPRINT_HPP)
# include LFPRINT_HPP
# define has_debug 1
#endif

#if __has_include(LFCOUNT_HPP)
# include LFCOUNT_HPP
#endif

#endif
