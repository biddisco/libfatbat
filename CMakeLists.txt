# ---------------------------------------------------------------------
# ---------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/project_libfabric.cmake)
include(FetchContent)

# ---------------------------------------------------------------------
# Boost dependency
# ---------------------------------------------------------------------
find_package(Boost REQUIRED CONFIG COMPONENTS thread)

# ---------------------------------------------------------------------
# hwmalloc dependency
# ---------------------------------------------------------------------
find_package(hwmalloc QUIET)
# Override the source directory for FetchContent's hwmalloc, leave binary dir as
# default
if(EXISTS "${PROJECT_SOURCE_DIR}/extern/hwmalloc")
  set(FETCHCONTENT_SOURCE_DIR_HWMALLOC "${PROJECT_SOURCE_DIR}/extern/hwmalloc")
endif()

if(NOT hwmalloc_FOUND)
  FetchContent_Declare(
    hwmalloc
    GIT_REPOSITORY https://github.com/ghex-org/hwmalloc.git
    GIT_TAG jb-develop)

  FetchContent_GetProperties(hwmalloc)
  if(NOT hwmalloc_POPULATED)
    message(STATUS "✅ hwmalloc unpopulated: fetching from source")
    FetchContent_MakeAvailable(hwmalloc)
    message(
      STATUS
        "✅ hwmalloc built from source (${hwmalloc_VERSION}) in ${hwmalloc_BINARY_DIR}"
    )
  endif()
endif()

# ---------------------------------------------------------------------
# dummy library of our private headers
# ---------------------------------------------------------------------
add_library(exampi_private_libfabric_headers INTERFACE)
target_include_directories(
  exampi_private_libfabric_headers
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

# ---------------------------------------------------------------------
# main library
# ---------------------------------------------------------------------
add_library(exampi_libfabric SHARED)
# actual library (created in exampi_libfabric.cmake), depends on dummy library
target_link_libraries(exampi_libfabric PRIVATE exampi_private_libfabric_headers)
target_link_libraries(exampi_libfabric PRIVATE Boost::thread)
target_link_libraries(exampi_libfabric PRIVATE HWMALLOC::hwmalloc)

# we need to include a binary dir for the exampi_config_defines.hpp file
target_include_directories(
  exampi_libfabric
  INTERFACE "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/src/libfabric>")

list(TRANSFORM exampi_sources PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/../
                                      OUTPUT_VARIABLE exampi_sources_libfabric)
target_sources(exampi_libfabric PRIVATE ${exampi_sources_libfabric})
# target_sources(exampi_libfabric PRIVATE context.cpp)
target_sources(exampi_libfabric PRIVATE operation_context.cpp)

# ------------------------------------------------------------------------------
# used by template expansion for location of print.hpp
# ------------------------------------------------------------------------------
set(EXAMPI_SRC_LIBFABRIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ------------------------------------------------------------------------------
# Write options to file in build dir
# ------------------------------------------------------------------------------
exampi_libfabric_write_config_defines_file(
  NAMESPACE libfabric FILENAME
  "${CMAKE_CURRENT_BINARY_DIR}/exampi_libfabric_defines.hpp" TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/libfabric_defines_template.hpp")

target_include_directories(exampi_libfabric
                           PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# ------------------------------------------------------------------------------
# if we are using GPU, then the libfabric library was probably built with gpu
# support, and we should link to cuda to prevent link errors
# ------------------------------------------------------------------------------
if(HWMALLOC_ENABLE_DEVICE)
  include(CheckLanguage)
  check_language(CUDA)

  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
  else()
    message(STATUS "No CUDA support")
    return()
  endif()

  find_package(CUDAToolkit)
  target_link_libraries(exampi_libfabric PRIVATE CUDA::cudart)
endif()
