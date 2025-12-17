include(FindPackageHandleStandardArgs)
find_package(PkgConfig QUIET)

if (PkgConfig_FOUND)
  pkg_check_modules(PMIx REQUIRED pmix)
endif()

if (NOT PMIx_FOUND)
  find_path(
    PMIx_INCLUDE_DIRS pmix.h
    HINTS ${PMIx_ROOT}
          ENV
          PMIx_ROOT
          ${PMIx_DIR}
          ENV
          PMIx_DIR
    PATH_SUFFIXES include
  )

  find_library(
    PMIx_LIBRARIES
    NAMES pmix
    HINTS ${PMIx_ROOT}
          ENV
          PMIx_ROOT
    PATH_SUFFIXES lib lib64
  )

  # Set PMIx_ROOT in case the other hints are used
  if(PMIx_ROOT)
    # The call to file is for compatibility with windows paths
    file(TO_CMAKE_PATH ${PMIx_ROOT} PMIx_ROOT)
  elseif("$ENV{PMIx_ROOT}")
    file(TO_CMAKE_PATH $ENV{PMIx_ROOT} PMIx_ROOT)
  else()
    file(TO_CMAKE_PATH "${PMIx_INCLUDE_DIRS}" PMIx_INCLUDE_DIRS)
    string(REPLACE "/include" "" PMIx_ROOT "${PMIx_INCLUDE_DIRS}")
  endif()

  if(NOT PMIx_LIBRARIES OR NOT PMIx_INCLUDE_DIRS)
    set(PMIx_FOUND=OFF)
    return()
  endif()
else()
  # pckconfig returns the var we want as a link library
  set(PMIx_LIBRARIES ${PMIx_LINK_LIBRARIES})
endif()

find_package_handle_standard_args(PMIx DEFAULT_MSG PMIx_LIBRARIES PMIx_INCLUDE_DIRS)

mark_as_advanced(PMIx_ROOT PMIx_LIBRARIES PMIx_INCLUDE_DIRS)

if(NOT TARGET PMIX::pmix AND PMIx_FOUND)
  add_library(PMIX::pmix SHARED IMPORTED)
  set_target_properties(PMIX::pmix PROPERTIES
    IMPORTED_LOCATION ${PMIx_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${PMIx_INCLUDE_DIRS}
  )
endif()

set(LIBFATBAT_PMI_LIBRARY PMIX::pmix)
