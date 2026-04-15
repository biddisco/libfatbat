/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <pmix.h>
// #include "examples.h"
#include "libfatbat/logging.hpp"

#define CHECK_PMIX(name, f)                                                                        \
  {                                                                                                \
    pmix_status_t rc = (f);                                                                        \
    if (PMIX_SUCCESS != rc)                                                                        \
    {                                                                                              \
      std::cerr << "PMIx_ " << name << " failed: " << PMIx_Error_string(rc) << std::endl;          \
      throw std::runtime_error("PMIx failure");                                                    \
    }                                                                                              \
  }

int main(int argc, char** argv)
{
  pmix_proc_t myproc;    // local process info
  pmix_proc_t proc;      // other process info
  pmix_value_t* val = NULL;
  uint32_t rank = -1;
  uint32_t node = -1;
  uint32_t size = -1;

#if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::trace);
#endif

  // init pmix
  CHECK_PMIX("PMIx Init", PMIx_Init(&myproc, NULL, 0));

  // copy myproc details into struct
  PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_INVALID);

  // get our rank
  CHECK_PMIX("Get Rank", PMIx_Get(&proc, PMIX_RANK, NULL, 0, &val));
  rank = val->data.uint32;
  PMIX_VALUE_RELEASE(val);

  // get our node ID
  CHECK_PMIX("Get Node", PMIx_Get(&myproc, PMIX_NODEID, NULL, 0, &val));
  node = val->data.uint32;
  PMIX_VALUE_RELEASE(val);

  // get size
  CHECK_PMIX("Get Size", PMIx_Get(&proc, PMIX_APP_SIZE, NULL, 0, &val));
  size = val->data.uint32;
  PMIX_VALUE_RELEASE(val);

  SPDLOG_DEBUG(
      "{:20} {} : Rank {}/{} : on Node {}", "Process", PMIx_Proc_string(&myproc), rank, size, node);

  // share a string across all ranks
  char key[PMIX_MAX_KEYLEN];
  char my_string[64];
  fmt::format_to(my_string, "Hello from rank {:03}\0", myproc.rank);
  fmt::format_to(key, "LIBFABRIC_{:03}_STRING\0", myproc.rank);
  SPDLOG_DEBUG("{:20} rank {} key {} value {}", "Prepared data", rank, key, my_string);

  pmix_value_t value;
  value.type = PMIX_STRING;
  value.data.string = my_string;

  {
    SPDLOG_SCOPE("Rank {} Setting key/value : {}/{}", rank, key, my_string);
    CHECK_PMIX("Put", PMIx_Put(PMIX_GLOBAL, key, &value));
    CHECK_PMIX("Commit", PMIx_Commit());
    CHECK_PMIX("Fence", PMIx_Fence(NULL, 0, NULL, 0));
  }

  if (rank == 0)
  {
    for (pmix_rank_t r = 0; r < size; r++)
    {
      pmix_proc_t proc;
      pmix_value_t* val = NULL;

      SPDLOG_SCOPE("Rank {} Getting data", rank);
      PMIX_LOAD_PROCID(&proc, myproc.nspace, r);
      fmt::format_to(key, "LIBFABRIC_{:03}_STRING\0", r);

      CHECK_PMIX("Get (key)", PMIx_Get(&proc, key, NULL, 0, &val));
      SPDLOG_DEBUG("{:20} Rank {} from rank {}: {}", "Key read", myproc.rank, r, val->data.string);
      PMIx_Value_free(val, 1);
    }
  }

  // finalize us
  CHECK_PMIX("PMIx Finalize", PMIx_Finalize(NULL, 0));
  return 0;
}
