# ------------------------------------------------------------------------------
# Find PMI2 / PMIx if possible, exit quietly if not found
# ------------------------------------------------------------------------------
find_package(PMI2 QUIET)

if(NOT PMI2_FOUND)
  find_package(PMIx QUIET)
endif()

# ------------------------------------------------------------------------------
# Find MPI if possible, ctest uses it to start jobs with PMI
# ------------------------------------------------------------------------------
find_package(MPI QUIET)

if(PMI2_FOUND OR PMIx_FOUND)
  add_subdirectory(pmi)
endif()
