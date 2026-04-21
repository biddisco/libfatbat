# ------------------------------------------------------------------------------
# PMI(x) tests which have one c++ file per exe
# ------------------------------------------------------------------------------
set(PMI_TEST_SRCS)
set(PMI_BENCH_SRCS)

if(PMIx_FOUND)
  list(APPEND PMI_TEST_SRCS kv-test-pmix pmi-boot-libfabric)
endif()

if(TARGET Boost::program_options)
  list(APPEND PMI_TEST_SRCS send-recv-test rdma-read-test)
  list(APPEND PMI_BENCH_SRCS send-recv-benchmark rdma-read-benchmark)
endif()

# ------------------------------------------------------------------------------
# function to create exes for tests
# ------------------------------------------------------------------------------
function(create_test_executable test COMPILE_ONLY)
  add_executable(${test} ${test}.cxx)

  if(NOT "${FATBAT_LOG_LEVEL_UPPERCASE}" MATCHES "OFF")
    target_link_libraries(${test} PUBLIC spdlog::spdlog)
    target_link_libraries(${test} PUBLIC fmt::fmt)
  endif()
  target_link_libraries(${test} PUBLIC libfatbat ${PMI_LIBRARY_TARGET})
  if(TARGET Boost::program_options)
    target_link_libraries(${test} PUBLIC libfatbat Boost::headers Boost::program_options)
  endif()
  # to include test utilities and test controller base class using "test/xxx.hpp"
  target_include_directories(${test} PUBLIC ${PROJECT_SOURCE_DIR})

  if(NOT COMPILE_ONLY)
    add_test(NAME ${test} COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
                                  ${MPIEXEC_PREFLAGS} $<TARGET_FILE:${test}> ${MPIEXEC_POSTFLAGS}
    )
  endif()
endfunction()

# ------------------------------------------------------------------------------
# create executables (built, but not run by ctest)
# ------------------------------------------------------------------------------
foreach(test ${PMI_TEST_SRCS})
  create_test_executable("${test}" FALSE)
endforeach()

foreach(bench ${PMI_BENCH_SRCS})
  create_test_executable("${bench}" TRUE)
endforeach()
