cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

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
set_policy(CMP0167 NEW) # Use BoostConfig and do not use old cmake find boost

# ------------------------------------------------------------------------------
# cmake includes
# ------------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(FetchContent)
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
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------
# Boost we use crc and lockfree, but these are header only not in COMPONENTS
# ---------------------------------------------------------------------
find_package(Boost REQUIRED CONFIG COMPONENTS thread)

# ---------------------------------------------------------------------
# hwmalloc dependency
# ---------------------------------------------------------------------
find_package(hwmalloc QUIET)
# If present, override source directory, leave binary dir as default
if(EXISTS "${CMAKE_SOURCE_DIR}/extern/hwmalloc")
  set(FETCHCONTENT_SOURCE_DIR_HWMALLOC "${CMAKE_SOURCE_DIR}/extern/hwmalloc")
endif()

if(NOT hwmalloc_FOUND)
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

# ------------------------------------------------------------------------------
# Performance counters
# ------------------------------------------------------------------------------
option(FATBAT_WITH_PERFORMANCE_COUNTERS "Enable libfabric performance counters (default: OFF)" OFF)

if(FATBAT_WITH_PERFORMANCE_COUNTERS)
  libfatbat_add_config_define_namespace(DEFINE FATBAT_HAVE_PERFORMANCE_COUNTERS NAMESPACE libfatbat)
endif()

# ---------------------------------------------------------------------
# currently header only interface library
# ---------------------------------------------------------------------
add_library(libfatbat INTERFACE)
target_link_libraries(libfatbat INTERFACE Boost::thread)
target_link_libraries(libfatbat INTERFACE HWMALLOC::hwmalloc)

target_include_directories(
  libfatbat INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>"
                      "$<INSTALL_INTERFACE:include>"
)
# we need to include a binary dir for the libfatbat_config_defines.hpp file
target_include_directories(libfatbat INTERFACE "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>")

# ------------------------------------------------------------------------------
# if we are using GPU, then the libfabric library was probably built with gpu support, and we should
# link to cuda to prevent link errors
# ------------------------------------------------------------------------------
if(HWMALLOC_ENABLE_DEVICE)
  target_compile_definitions(libfatbat PUBLIC LIBFATBAT_ENABLE_DEVICE)
  include(CheckLanguage)
  check_language(CUDA)

  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
  else()
    message(STATUS "No CUDA support")
    return()
  endif()

  find_package(CUDAToolkit)
  target_link_libraries(libfatbat PRIVATE CUDA::cudart)
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

# target_include_directories(libfatbat PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
