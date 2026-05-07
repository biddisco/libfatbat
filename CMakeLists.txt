cmake_minimum_required(VERSION 3.22 FATAL_ERROR)

# ------------------------------------------------------------------------------
# Define libFatbat Project
# ------------------------------------------------------------------------------
project(libFatbat VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------------------
# Policies
# ------------------------------------------------------------------------------
function(set_policy POLICY_NAME POLICY_VALUE)
  if(POLICY ${POLICY_NAME})
    cmake_policy(SET ${POLICY_NAME} ${POLICY_VALUE})
  endif()
endfunction()

set_policy(CMP0028 NEW) # Double colon in target name means ALIAS or IMPORTED target
set_policy(CMP0060 NEW) # Link libraries by full path even in implicit directories
set_policy(CMP0074 NEW) # find_package uses XXX_ROOT vars using PackageName
set_policy(CMP0144 NEW) # find_package uses XXX_ROOT vars using PACKAGENAME Uppercase.
set_policy(CMP0167 NEW) # Use BoostConfig and do not use old cmake find boost

# ------------------------------------------------------------------------------
# Support testing
# ------------------------------------------------------------------------------
include(CTest)
enable_testing()

# ------------------------------------------------------------------------------
# cmake includes
# ------------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(CMakeDependentOption)
include(FetchContent)
include(Messages)
include(libfatbat_macros)

# ------------------------------------------------------------------------------
# Setup Output directories
# ------------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
    CACHE PATH "Single Directory for all Executables."
)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib
    CACHE PATH "Single Directory for all Libraries"
)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib
    CACHE PATH "Single Directory for all static libraries."
)

# ---------------------------------------------------------------------
# Enable Position Independent Code (PIC) for all targets
# ---------------------------------------------------------------------
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------
# log level, default os off
set(FATBAT_LOG_LEVEL "off" CACHE STRING "Enable logging for messages of >=level")
set_property(
  CACHE FATBAT_LOG_LEVEL
  PROPERTY STRINGS
           off
           trace
           debug
           info
           warn
           error
           critical
)

# ---------------------------------------------------------------------
# options
# ---------------------------------------------------------------------
option(FATBAT_WITH_COUNTERS "Enable internal counters (default: OFF)" ON)
option(FATBAT_WITH_HWMALLOC "Enable hwmalloc pinned memory heaps (default: OFF)" OFF)
option(FATBAT_WITH_GPU_SUPPORT "Enable device support (default: OFF)" ON)

# ---------------------------------------------------------------------
# Libfabric
# ---------------------------------------------------------------------
find_package(Libfabric REQUIRED)
colour_message(STATUS ${Green} "✅ Libfabric found ${Libfabric_VERSION}")

# ---------------------------------------------------------------------
# Boost we use crc and lockfree, but these are header only
# ---------------------------------------------------------------------
find_package(Boost REQUIRED CONFIG COMPONENTS headers)
# We optionally use program options in some benchmarks/tests
find_package(Boost QUIET CONFIG COMPONENTS headers program_options)
if(TARGET Boost::program_options)
  libfatbat_add_config_define_namespace(
    DEFINE LIBFATBAT_HAVE_BOOST_PROGRAM_OPTIONS NAMESPACE libfatbat
  )
endif()

# ------------------------------------------------------------------------------
# spdlog logging library
# ------------------------------------------------------------------------------
string(TOUPPER ${FATBAT_LOG_LEVEL} FATBAT_LOG_LEVEL_UPPERCASE)
if(NOT "${FATBAT_LOG_LEVEL_UPPERCASE}" MATCHES "OFF")
  find_package(spdlog QUIET)
  if(spdlog_FOUND)
    colour_message(STATUS ${Green} "✅ spdlog found ${spdlog_VERSION}")
    message(STATUS "spdlog include dirs: ${spdlog_INCLUDE_DIRS}")
  else()
    FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v2.x)
    FetchContent_MakeAvailable(spdlog)
    colour_message(STATUS ${Green} "✅ Fetched spdlog ${spdlog_VERSION}")
  endif()
endif()

# ------------------------------------------------------------------------------
# Shim to alias libnuma if found already using mscclpp
# ------------------------------------------------------------------------------
if(TARGET MSCCLPP::numa)
  add_library(NUMA::libnuma ALIAS MSCCLPP::numa)
endif()

# ------------------------------------------------------------------------------
# Before adding HWMALLOC - set GPU support accordingly
# ------------------------------------------------------------------------------
if(FATBAT_WITH_GPU_SUPPORT)
  libfatbat_add_config_define_namespace(DEFINE LIBFATBAT_HAVE_GPU_SUPPORT NAMESPACE libfatbat)
endif()
set(HWMALLOC_ENABLE_DEVICE ${FATBAT_WITH_GPU_SUPPORT}
    CACHE BOOL "Enable device support in hwmalloc" FORCE
)
mark_as_advanced(HWMALLOC_ENABLE_DEVICE)

# ---------------------------------------------------------------------
# hwmalloc dependency
# ---------------------------------------------------------------------
if(FATBAT_WITH_HWMALLOC)
  find_package(hwmalloc QUIET)
  if(hwmalloc_FOUND OR TARGET hwmalloc)
    colour_message(STATUS ${Green} "✅ hwmalloc found ${hwmalloc_VERSION}")
  else()
    FetchContent_Declare(
      hwmalloc GIT_REPOSITORY https://github.com/ghex-org/hwmalloc.git GIT_TAG jb-develop
    )

    FetchContent_GetProperties(hwmalloc)
    if(NOT hwmalloc_POPULATED)
      message(STATUS "✅ hwmalloc unpopulated: fetching: ${FETCHCONTENT_SOURCE_DIR_HWMALLOC}")
      FetchContent_MakeAvailable(hwmalloc)
      message(STATUS "✅ hwmalloc building: (${hwmalloc_VERSION}) in ${hwmalloc_BINARY_DIR}")
    endif()
  endif()
  libfatbat_add_config_define_namespace(DEFINE LIBFATBAT_HAVE_HWMALLOC NAMESPACE libfatbat)
endif()

# If present, override source directory, leave binary dir as default
if(NOT TARGET hwmalloc AND EXISTS "${CMAKE_SOURCE_DIR}/extern/hwmalloc"
   AND NOT FETCHCONTENT_SOURCE_DIR_HWMALLOC
)
  set(FETCHCONTENT_SOURCE_DIR_HWMALLOC "${CMAKE_SOURCE_DIR}/extern/hwmalloc")
endif()

# ------------------------------------------------------------------------------
# Performance counters
# ------------------------------------------------------------------------------
if(FATBAT_WITH_COUNTERS)
  libfatbat_add_config_define_namespace(DEFINE LIBFATBAT_HAVE_COUNTERS NAMESPACE libfatbat)
endif()

# ---------------------------------------------------------------------
# libfatbat itself is currently header only interface library
# ---------------------------------------------------------------------
add_library(libfatbat INTERFACE)
target_link_libraries(libfatbat INTERFACE Boost::headers)
target_link_libraries(libfatbat INTERFACE $<$<BOOL:${FATBAT_WITH_HWMALLOC}>:HWMALLOC::hwmalloc>)
target_link_libraries(libfatbat INTERFACE libfabric::libfabric)

# add spdlog and fmt if logging is enabled and set compile definitions for logging
if(NOT "${FATBAT_LOG_LEVEL_UPPERCASE}" MATCHES "OFF")
  target_compile_definitions(libfatbat INTERFACE LIBFATBAT_LOGGING_ENABLED=1)
  target_compile_definitions(
    libfatbat INTERFACE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_${FATBAT_LOG_LEVEL_UPPERCASE}
  )
  target_link_libraries(
    libfatbat INTERFACE $<BUILD_INTERFACE:spdlog::spdlog> $<BUILD_INTERFACE:fmt::fmt>
  )
endif()

target_include_directories(
  libfatbat INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>"
                      "$<INSTALL_INTERFACE:include>"
)
# we need to include a binary dir for the libfatbat_config_defines.hpp file
target_include_directories(libfatbat INTERFACE "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>")

# ------------------------------------------------------------------------------
# if we are using GPU, then the libfabric library was probably built with gpu support, and we should
# link to cuda to prevent link errors
#
# TODO - check for HIP gpu support in hwmalloc and use that instead
# ------------------------------------------------------------------------------
if(HWMALLOC_ENABLE_DEVICE)
  # @TODO - add libfabric device options and select cuda/hip/other
  # target_compile_definitions(libfatbat INTERFACE LIBFATBAT_ ENABLE_DEVICE)
  target_compile_definitions(libfatbat INTERFACE LIBFATBAT_HAVE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)

  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
  else()
    message(STATUS "No CUDA support")
    return()
  endif()

  find_package(CUDAToolkit)
  target_link_libraries(libfatbat INTERFACE CUDA::cudart)
endif()

# ------------------------------------------------------------------------------
# used by template expansion for location of print.hpp
# ------------------------------------------------------------------------------
set(LIBFATBAT_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ------------------------------------------------------------------------------
# Write options to file in build dir
# ------------------------------------------------------------------------------
libfatbat_write_config_defines_file(
  NAMESPACE libfatbat FILENAME "${CMAKE_CURRENT_BINARY_DIR}/libfatbat_defines.hpp" TEMPLATE
  "cmake/libfatbat_defines_template.hpp"
)
add_subdirectory(test)
