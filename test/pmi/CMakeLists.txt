# ------------------------------------------------------------------------------
# PMI(x) tests which have one c++ file per exe
# ------------------------------------------------------------------------------
set(PMI_TEST_SRCS)
set(PMI_BENCH_SRCS)

if(PMIx_FOUND)
  list(APPEND PMI_TEST_SRCS kv-test-pmix pmi-boot-libfabric)
elseif(PMI2_FOUND)
  list(APPEND PMI_TEST_SRCS pmi-boot-libfabric)
endif()

if(TARGET Boost::program_options AND FATBAT_WITH_HWMALLOC)
  list(APPEND PMI_TEST_SRCS send-recv-test rdma-read-test rdma-write-test rdma-writedata-test)
  list(APPEND PMI_BENCH_SRCS send-recv-benchmark rdma-read-benchmark rdma-write-benchmark
       rdma-writedata-benchmark
  )
  set(send-recv-benchmark_PARAMS "-i" "2")
  set(rdma-read-benchmark_PARAMS "-i" "2")
  set(rdma-write-benchmark_PARAMS "-i" "2")
  set(rdma-writedata-benchmark_PARAMS "-i" "2")
endif()

if(FATBAT_WITH_GPU_SUPPORT AND FATBAT_WITH_HWMALLOC)
  list(APPEND PMI_TEST_SRCS rdma-read-test-device)
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
    if(DEFINED ${test}_PARAMS)
      set(test_params "${${test}_PARAMS}")
    else()
      set(test_params "")
    endif()
    add_test(NAME ${test}
             COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS}
                     $<TARGET_FILE:${test}> ${MPIEXEC_POSTFLAGS} ${test_params}
    )
  endif()
endfunction()

# ------------------------------------------------------------------------------
# create executables (built, but not run by ctest)
# ------------------------------------------------------------------------------
foreach(test ${PMI_TEST_SRCS})
  create_test_executable("${test}" FALSE)
endforeach()

# will will add the benchmarks as tests but use small number of iterations to keep the test time
# short.
foreach(bench ${PMI_BENCH_SRCS})
  create_test_executable("${bench}" FALSE)
endforeach()
