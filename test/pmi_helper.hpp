/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>
//
#ifdef FATBAT_PMI2_ENABLED
# include <pmi2.h>
#endif
#ifdef FATBAT_PMIx_ENABLED
# include <pmix.h>
#endif

// libfatbat includes
#include "libfatbat/controller_base.hpp"
#include "libfatbat/locality.hpp"
#include "libfatbat/logging.hpp"

// boost : base64 encode/decode (for exchanging addresses via PMI KVS)
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
using namespace boost::archive::iterators;
typedef base64_from_binary<transform_width<std::string::const_iterator, 6, 8>> base64_t;
typedef transform_width<binary_from_base64<std::string::const_iterator>, 8, 6> binary_t;

// ------------------------------------------------------------------
inline auto pmihelp_log = libfatbat::log::create("PMIHelp");

// --------------------------------------------------------------------
// When running on an HPC system, using mpi or slurm for launch,
// PMI will usually be present and we can use it for booting.
// This class encapsulates some basic PMI init/finalize functionality
// and Key/Value store operations for libfabric address exchange.
//
// There are two implementations, one for PMI2 and one for PMIx.
// --------------------------------------------------------------------
struct pmi_helper
{
  uint32_t rank = -1;
  uint32_t node = -1;
  uint32_t size = -1;
#ifdef FATBAT_PMIx_ENABLED
  pmix_proc_t myproc;
#endif

  // --------------------------------------------------------------------
  void debug_hook(bool attach_debugger)
  {
    if (rank == 0 && attach_debugger)
    {
      std::cout << "Please attach debugger and hit return" << std::endl;
      char c;
      std::cin >> c;
    }
    fence();
  }

  // --------------------------------------------------------------------
  // Implementation that is used when PMI2 is used on the system
  // --------------------------------------------------------------------
#ifdef FATBAT_PMI2_ENABLED
  // --------------------------------------------------------------------
  void fence() { PMI2_KVS_Fence(); }

  // --------------------------------------------------------------------
  std::tuple<uint32_t, uint32_t> init_PMI(bool attach_debugger)
  {
    int spawned;
    int appnum;
    LIBFATBAT_SCOPE(pmihelp_log, "{}", "PMI init");
    PMI2_Init(&spawned, (int*) &size, (int*) &rank, &appnum);

    debug_hook(attach_debugger);
    return std::make_tuple(rank, size);
  }

  // --------------------------------------------------------------------
  void finalize_PMI()
  {
    PMI2_Finalize();
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} on rank {:04}", "PMI finalized", rank);
  }

  // --------------------------------------------------------------------
  template <typename Derived, typename OperationContext>
  void boot_PMI(libfatbat::controller_base<Derived, OperationContext>* controller)
  {
    using namespace libfatbat;

    // we must pass our libfabric data to other nodes
    // encode it as a string to put into the PMI KV store
    auto here = controller->here();
    std::string encoded_locality(base64_t((char const*) (here.fabric_data().data())),
        base64_t((char const*) (here.fabric_data().data()) + locality_defs::array_size));
    int encoded_length = encoded_locality.size();
    LIBFATBAT_DEBUG(
        pmihelp_log, "{:<20} {} : length {}", "Encoded locality", encoded_locality, encoded_length);

    // Key name for PMI
    std::string pmi_key = "LIBFABRIC_" + std::to_string(rank);
    // insert our data in the KV store
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} on rank {:04}", "PMI2_KVS_Put", rank);
    PMI2_KVS_Put(pmi_key.data(), encoded_locality.data());

    // Wait for all to do the same
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} on rank {:04}", "PMI2_KVS_Fence", rank);
    PMI2_KVS_Fence();

    // read libfabric data for all nodes and insert into our address vector
    for (int i = 0; i < size; ++i)
    {
      locality new_locality;
      if (i != rank)
      {
        // read one locality key
        std::string pmi_key = "LIBFABRIC_" + std::to_string(i);
        char encoded_data[locality_defs::array_size * 2];
        int length = 0;
        PMI2_KVS_Get(0, i, pmi_key.data(), encoded_data, encoded_length + 1, &length);
        if (length != encoded_length)
        {
          LIBFATBAT_ERROR(
              pmihelp_log, "PMI value length mismatch, expected {} got {}", encoded_length, length);
        }
        // decode the string back to raw locality data
        LIBFATBAT_DEBUG(pmihelp_log, "{:<20} for rank {} on rank {:04}", "address decode", i, rank);
        std::copy(binary_t(encoded_data), binary_t(encoded_data + encoded_length),
            (new_locality.fabric_data_writable()));
      }
      else
      {
        new_locality = here;
      }

      // insert locality into address vector
      LIBFATBAT_DEBUG(pmihelp_log, "{:<20} for rank {} on rank {:04}", "insert_address", i, rank);
      new_locality = controller->insert_address(new_locality);
    }
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} on rank {:04}", "Completed boot_PMI", rank);
  }
#endif

  // --------------------------------------------------------------------
  // Implementation that is used when PMIx is used on the system
  // --------------------------------------------------------------------
#ifdef FATBAT_PMIx_ENABLED

# define CHECK_PMIX(name, f)                                                                       \
   {                                                                                               \
     pmix_status_t rc = (f);                                                                       \
     if (PMIX_SUCCESS != rc)                                                                       \
     {                                                                                             \
       std::cerr << "PMIx_ " << name << " failed: " << PMIx_Error_string(rc) << std::endl;         \
       throw std::runtime_error("PMIx failure");                                                   \
     }                                                                                             \
   }

  // --------------------------------------------------------------------
  void fence() { CHECK_PMIX("PMIx Fence", PMIx_Fence(NULL, 0, NULL, 0)); }

  // --------------------------------------------------------------------
  std::tuple<uint32_t, uint32_t> init_PMI(bool attach_debugger)
  {
    pmix_proc_t proc;    // other process info
    pmix_value_t* val = NULL;

    // init pmix
    CHECK_PMIX("PMIx Init", PMIx_Init(&myproc, NULL, 0));

    // copy myproc details into struct
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_INVALID);

    // get our rank
    CHECK_PMIX("Get Rank", PMIx_Get(&proc, PMIX_RANK, NULL, 0, &val));
    rank = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    debug_hook(attach_debugger);

    // get our node ID
    CHECK_PMIX("Get Node", PMIx_Get(&myproc, PMIX_NODEID, NULL, 0, &val));
    node = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    // get size
    CHECK_PMIX("Get Size", PMIx_Get(&proc, PMIX_APP_SIZE, NULL, 0, &val));
    size = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    LIBFATBAT_DEBUG(pmihelp_log, "{:<20}: {} Rank {}/{} : on Node {}", "Process",
        PMIx_Proc_string(&myproc), rank, size, node);
    return std::make_tuple(rank, size);
  }

  // --------------------------------------------------------------------
  void finalize_PMI()
  {
    // finalize us
    CHECK_PMIX("PMIx Finalize", PMIx_Finalize(NULL, 0));
  }

  // --------------------------------------------------------------------
  template <typename Derived, typename OperationContext>
  void boot_PMI(libfatbat::controller_base<Derived, OperationContext>* controller)
  {
    using namespace libfatbat;

    // we must pass our libfabric address to other nodes
    // encode it as a string to put into the PMI KV store
    auto here = controller->here();
    std::string encoded_address = here.to_str();
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} {} : rank {:04}, length {}", "Locality string",
        encoded_address, rank, encoded_address.size());

    std::string encoded_locality(base64_t((char const*) (here.fabric_data().data())),
        base64_t((char const*) (here.fabric_data().data()) + locality_defs::array_size));
    int encoded_length = encoded_locality.size();
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} {} {} ({})", "Encoded locality", encoded_locality,
        encoded_length, here.to_str());

    // Key name for PMI
    std::string pmi_key = "LIBFABRIC_" + std::to_string(rank);
    // insert our data in the KV store
    LIBFATBAT_DEBUG(
        pmihelp_log, "{:<20} on rank {:04} {} {}", "PMIx_KVS_Put", rank, pmi_key, encoded_locality);

    // share {key,value} across all ranks
    pmix_value_t value;
    value.type = PMIX_STRING;
    value.data.string = (char*) encoded_locality.c_str();

    {
      LIBFATBAT_SCOPE(pmihelp_log, "{} {}", "Putting data", rank);
      CHECK_PMIX("Put", PMIx_Put(PMIX_GLOBAL, pmi_key.c_str(), &value));
      CHECK_PMIX("Commit", PMIx_Commit());
      LIBFATBAT_DEBUG(pmihelp_log, "{:<20} on rank {:04}", "PMIx Fence", rank);
      CHECK_PMIX("Fence", PMIx_Fence(NULL, 0, NULL, 0));
    }

    // read libfabric data for all nodes and insert into our address vector
    for (pmix_rank_t r = 0; r < size; r++)
    {
      locality new_locality;
      if (r != rank)
      {
        LIBFATBAT_SCOPE(pmihelp_log, "{} {}", "Getting data from/for", rank, r);
        pmix_proc_t proc;
        pmix_value_t* val = NULL;
        //
        PMIX_LOAD_PROCID(&proc, myproc.nspace, r);
        std::string pmi_key = "LIBFABRIC_" + std::to_string(r);
        CHECK_PMIX("Get (key)", PMIx_Get(&proc, pmi_key.c_str(), NULL, 0, &val));
        std::string encoded_address = val->data.string;
        LIBFATBAT_DEBUG(pmihelp_log, "{:<20} rank {:04} received {} from rank {:04}", "PMI boot",
            myproc.rank, encoded_address, r);
        PMIx_Value_free(val, 1);
        //
        int length = encoded_address.size();
        assert(length == encoded_length);
        // decode the string back to raw locality data
        LIBFATBAT_DEBUG(pmihelp_log, "{:<20} rank {:04} decode from rank {:04}", "address decode",
            myproc.rank, r);
        std::copy(binary_t(encoded_address.begin()), binary_t(encoded_address.end()),
            (new_locality.fabric_data_writable()));
      }
      else
      {
        new_locality = here;
      }
      // insert locality into address vector
      LIBFATBAT_DEBUG(pmihelp_log, "{:<20} rank {:04} decode from rank {:04}", "insert_address",
          myproc.rank, r);
      new_locality = controller->insert_address(new_locality);
    }
    LIBFATBAT_DEBUG(pmihelp_log, "{:<20} on rank {:04}", "Completed boot_PMI", rank);
  }
#endif
};
