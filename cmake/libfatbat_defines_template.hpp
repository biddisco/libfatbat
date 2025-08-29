#ifndef LIBFATBAT_CONFIG_LIBFABRIC_HPP
#define LIBFATBAT_CONFIG_LIBFABRIC_HPP

// definitions that cmake generates from user options
// clang-format off

#define LIBFATBAT_HAVE_PERFORMANCE_COUNTERS
#define HAVE_LIBFABRIC_PROVIDER "tcp"
#define HAVE_LIBFABRIC_TCP

// clang-format on

// ------------------------------------------------------------------
// This section exists to make interoperabily/sharing of code easier -
// there are some files that do
// the majority of libfabric initialization/setup and polling that
// are basically the same in many apps, these files can be reused provided
// some namespaces for the lib and for debugging are setup correctly

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
