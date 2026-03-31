include(FindPackageHandleStandardArgs)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(PMIx REQUIRED pmix)
endif()

message(STATUS "PMIx_FOUND: ${PMIx_FOUND}")
if(PMIx_FOUND)
  message(STATUS "PMIx_VERSION: ${PMIx_VERSION}")
  set(PMIx_LIBRARIES ${PMIx_LINK_LIBRARIES})
else()
  find_path(PMIx_INCLUDE_DIRS pmix.h HINTS ${PMIx_ROOT} ENV PMIx_ROOT ${PMIx_DIR} ENV PMIx_DIR
            PATH_SUFFIXES include
  )

  find_library(PMIx_LIBRARIES NAMES pmix HINTS ${PMIx_ROOT} ENV PMIx_ROOT PATH_SUFFIXES lib lib64)

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
endif()

find_package_handle_standard_args(PMIx DEFAULT_MSG PMIx_LIBRARIES PMIx_INCLUDE_DIRS)

mark_as_advanced(PMIx_ROOT PMIx_LIBRARIES PMIx_INCLUDE_DIRS)

if(NOT TARGET PMIX::pmix AND PMIx_FOUND)
  add_library(PMIX::pmix SHARED IMPORTED)
  set_target_properties(
    PMIX::pmix PROPERTIES IMPORTED_LOCATION ${PMIx_LIBRARIES} INTERFACE_INCLUDE_DIRECTORIES
                                                              ${PMIx_INCLUDE_DIRS}
  )
  target_include_directories(PMIX::pmix INTERFACE ${PMIx_INCLUDE_DIRS})
  target_compile_definitions(PMIX::pmix INTERFACE "FATBAT_PMIx_ENABLED")
  set(PMI_LIBRARY_TARGET PMIX::pmix)
endif()
