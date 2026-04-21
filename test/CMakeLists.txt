# ------------------------------------------------------------------------------
# Find PMI2 / PMIx if possible, exit quietly if not found
# ------------------------------------------------------------------------------
find_package(PMI2 QUIET)

if(PMI2_FOUND)
  target_compile_definitions(${PMI_LIBRARY_TARGET} INTERFACE "LIBFATBAT_PMI2_ENABLED")
  colour_message(STATUS ${Green} "✅ Found PMI2 ${PMI_LIBRARY_TARGET}")
else()
  find_package(PMIx QUIET)
  if(PMIx_FOUND)
    target_compile_definitions(${PMI_LIBRARY_TARGET} INTERFACE "LIBFATBAT_PMIx_ENABLED")
    colour_message(STATUS ${Green} "✅ Found PMIx ${PMI_LIBRARY_TARGET}")
  endif()
endif()

# ------------------------------------------------------------------------------
# Find MPI if possible, ctest uses it to start jobs with PMI
# ------------------------------------------------------------------------------
find_package(MPI QUIET)

# if((PMI2_FOUND OR PMIx_FOUND) AND FATBAT_ENABLE_PMI_TESTS)
if((PMI2_FOUND OR PMIx_FOUND))
  add_subdirectory(pmi)
endif()
