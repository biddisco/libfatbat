# ------------------------------------------------------------------------------
# PMI(x) tests which have one c++ file per exe
# ------------------------------------------------------------------------------
set(PMI_TEST_SRCS pmi-boot-libfabric send-recv-test)

if(PMIx_FOUND)
  list(APPEND PMI_TEST_SRCS kv-test-pmix)
endif()

# ------------------------------------------------------------------------------
# create exes for tests
# ------------------------------------------------------------------------------
foreach(test ${PMI_TEST_SRCS})
  add_executable(${test} ${test}.cxx)
  target_include_directories(
    ${test} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/.."
  )

  if(NOT "${FATBAT_LOG_LEVEL_UPPERCASE}" MATCHES "OFF")
    target_link_libraries(${test} PUBLIC spdlog::spdlog)
    target_link_libraries(${test} PUBLIC fmt::fmt)
  endif()
  if(PMI2_FOUND)
    target_compile_definitions(${test} PUBLIC "FATBAT_PMI2_ENABLED")
    target_link_libraries(${test} PUBLIC libfatbat PMI2::pmi2)
  elseif(PMIx_FOUND)
    target_compile_definitions(${test} PUBLIC "FATBAT_PMIx_ENABLED")
    target_link_libraries(${test} PUBLIC libfatbat PMIX::pmix)
  endif()
  target_link_libraries(${test} PUBLIC libfatbat Boost::headers Boost::program_options)

  add_test(NAME ${test} COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS}
                                $<TARGET_FILE:${test}> ${MPIEXEC_POSTFLAGS}
  )
endforeach()
