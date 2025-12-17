find_package(PkgConfig QUIET)

# look for cray pmi...
pkg_check_modules(PC_PMI_CRAY QUIET cray-pmi)

# look for the rest if we couldn't find the cray package
if(NOT PC_PMI_CRAY_FOUND)
  pkg_check_modules(PC_PMI QUIET pmi)
endif()

message(STATUS "PMI: PkgConfig PC_PMI_CRAY_FOUND is ${PC_PMI_CRAY_FOUND}")
message(STATUS "PMI: PkgConfig PC_PMI_FOUND is ${PC_PMI_FOUND}")

find_path(
  PMI_INCLUDE_DIR pmi2.h
  HINTS ${PMI_ROOT}
        ENV
        PMI_ROOT
        ${PMI_DIR}
        ENV
        PMI_DIR
        ${PC_PMI_CRAY_INCLUDEDIR}
        ${PC_PMI_CRAY_INCLUDE_DIRS}
        ${PC_PMI_INCLUDEDIR}
        ${PC_PMI_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

find_library(
  PMI_LIBRARY
  NAMES pmi
  HINTS ${PMI_ROOT}
        ENV
        PMI_ROOT
        ${PC_PMI_CRAY_LIBDIR}
        ${PC_PMI_CRAY_LIBRARY_DIRS}
        ${PC_PMI_LIBDIR}
        ${PC_PMI_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64
)

# Set PMI_ROOT in case the other hints are used
if(PMI_ROOT)
  # The call to file is for compatibility with windows paths
  file(TO_CMAKE_PATH ${PMI_ROOT} PMI_ROOT)
elseif("$ENV{PMI_ROOT}")
  file(TO_CMAKE_PATH $ENV{PMI_ROOT} PMI_ROOT)
else()
  file(TO_CMAKE_PATH "${PMI_INCLUDE_DIR}" PMI_INCLUDE_DIR)
  string(REPLACE "/include" "" PMI_ROOT "${PMI_INCLUDE_DIR}")
endif()

if(NOT PMI_LIBRARY OR NOT PMI_INCLUDE_DIR)
  set(PMI_FOUND=OFF)
  return()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PMI2 DEFAULT_MSG PMI_LIBRARY PMI_INCLUDE_DIR)

mark_as_advanced(PMI_ROOT PMI_LIBRARY PMI_INCLUDE_DIR)

if(NOT TARGET PMI2::pmi2 AND PMI2_FOUND)
  add_library(PMI2::pmi2 SHARED IMPORTED)
  set_target_properties(PMI2::pmi2 PROPERTIES
    IMPORTED_LOCATION ${PMI_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${PMI_INCLUDE_DIR}
  )
endif()
