#pragma once

#ifdef FATBAT_PMI2_ENABLED
# include <pmi2.h>
#endif
#ifdef FATBAT_PMIx_ENABLED
# include <pmix.h>
#endif

#include "libfatbat/controller_base.hpp"
#include "libfatbat/locality.hpp"
#include "libfatbat/logging.hpp"

// base64 encode/decode
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
using namespace boost::archive::iterators;
typedef base64_from_binary<transform_width<std::string::const_iterator, 6, 8>> base64_t;
typedef transform_width<binary_from_base64<std::string::const_iterator>, 8, 6> binary_t;

// --------------------------------------------------------------------
class test_controller : public libfatbat::controller_base<test_controller>
{
  public:
  // --------------------------------------------------------------------
  void initialize_derived(std::string const& provider, bool rootnode, int size, size_t threads) {}

  // --------------------------------------------------------------------
  constexpr fi_threading threadlevel_flags()
  {
#if defined(HAVE_LIBFATBAT_GNI) || defined(HAVE_LIBFATBAT_LNX)
    return FI_THREAD_ENDPOINT;
#else
    return FI_THREAD_SAFE;
#endif
  }

  // --------------------------------------------------------------------
  uint64_t caps_flags(uint64_t /*available_flags*/) const
  {
    uint64_t flags_required = FI_TAGGED;
#ifndef HAVE_LIBFATBAT_LNX
    flags_required |= FI_MSG | FI_TAGGED | FI_RECV | FI_SEND | FI_RMA | FI_READ | FI_WRITE |
        FI_REMOTE_READ | FI_REMOTE_WRITE;
# if LIBFATBAT_ENABLE_DEVICE
    flags_required |= FI_HMEM;
# endif
#endif
    return flags_required;
  }

  // --------------------------------------------------------------------
  // we do not need to perform any special actions on init (to contact root node)
  void setup_root_node_address(struct fi_info* /*info*/)
  {    //
  }

  // --------------------------------------------------------------------
  // we do not need to perform any special actions on init (to init address)
  struct fi_info* set_src_dst_addresses(struct fi_info* info, bool tx)
  {    //
    return fi_dupinfo(info);
  }

  // --------------------------------------------------------------------
  // When running on an HPC system, using mpi or slurm for launch,
  // PMI will usually be present and we can use it for booting.
  // All nodes can share their addresses using this function
  // --------------------------------------------------------------------
#ifdef FATBAT_PMI2_ENABLED
  void boot_PMI(int rank, int size)
  {
    using namespace libfatbat;

    // we must pass our libfabric data to other nodes
    // encode it as a string to put into the PMI KV store
    std::string encoded_locality(base64_t((char const*) (here_.fabric_data().data())),
        base64_t((char const*) (here_.fabric_data().data()) + locality_defs::array_size));
    int encoded_length = encoded_locality.size();
    SPDLOG_DEBUG("{:20} {} : length {}", "Encoded locality", encoded_locality, encoded_length);

    // Key name for PMI
    std::string pmi_key = "LIBFABRIC_" + std::to_string(rank);
    // insert our data in the KV store
    SPDLOG_DEBUG("{:20} on rank {:04}", "PMI2_KVS_Put", rank);
    PMI2_KVS_Put(pmi_key.data(), encoded_locality.data());

    // Wait for all to do the same
    SPDLOG_DEBUG("{:20} on rank {:04}", "PMI2_KVS_Fence", rank);
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
          SPDLOG_ERROR("PMI value length mismatch, expected {} got {}", encoded_length, length);
        }
        // decode the string back to raw locality data
        SPDLOG_DEBUG("{:20} for rank {} on rank {:04}", "address decode", i, rank);
        std::copy(binary_t(encoded_data), binary_t(encoded_data + encoded_length),
            (new_locality.fabric_data_writable()));
      }
      else { new_locality = here_; }

      // insert locality into address vector
      SPDLOG_DEBUG("{:20} for rank {} on rank {:04}", "insert_address", i, rank);
      new_locality = insert_address(av_, new_locality);
      if (i == 0) { root_ = new_locality; }
    }

    PMI2_Finalize();
    SPDLOG_DEBUG("{:20} on rank {:04}", "PMI finalized", rank);
  }
#endif

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

  void boot_PMI(pmix_proc_t& myproc, int rank, int size)
  {
    using namespace libfatbat;

    // we must pass our libfabric address to other nodes
    // encode it as a string to put into the PMI KV store
    std::string encoded_locality(base64_t((char const*) (here_.fabric_data().data())),
        base64_t((char const*) (here_.fabric_data().data()) + locality_defs::array_size));
    int encoded_length = encoded_locality.size();
    SPDLOG_DEBUG("{:20} {} {} ({})", "Encoded locality as", encoded_locality, encoded_length,
        here_.to_str());

    // Key name for PMI
    std::string pmi_key = "LIBFABRIC_" + std::to_string(rank);
    // insert our data in the KV store
    SPDLOG_DEBUG("{:20} on rank {:04}", "PMI2_KVS_Put", rank);

    // share {key,value} across all ranks
    pmix_value_t value;
    value.type = PMIX_STRING;
    value.data.string = (char*) encoded_locality.c_str();

    {
      SPDLOG_SCOPE("Putting data", rank);
      CHECK_PMIX("Put", PMIx_Put(PMIX_GLOBAL, pmi_key.c_str(), &value));
      CHECK_PMIX("Commit", PMIx_Commit());
      SPDLOG_DEBUG("{:20} on rank {:04}", "PMIx Fence", rank);
      CHECK_PMIX("Fence", PMIx_Fence(NULL, 0, NULL, 0));
    }

    // read libfabric data for all nodes and insert into our address vector
    for (pmix_rank_t r = 0; r < size; r++)
    {
      locality new_locality;
      if (r != rank)
      {
        SPDLOG_SCOPE("Getting data from/for {}/{}", rank, r);
        pmix_proc_t proc;
        pmix_value_t* val = NULL;
        //
        PMIX_LOAD_PROCID(&proc, myproc.nspace, r);
        std::string pmi_key = "LIBFABRIC_" + std::to_string(r);
        CHECK_PMIX("Get (key)", PMIx_Get(&proc, pmi_key.c_str(), NULL, 0, &val));
        std::string encoded_address = val->data.string;
        SPDLOG_DEBUG("{:20} rank {:04} received {} from rank {:04}", "PMI boot", myproc.rank,
            encoded_address, r);
        PMIx_Value_free(val, 1);
        //
        int length = encoded_address.size();
        assert(length == encoded_length);
        // decode the string back to raw locality data
        SPDLOG_DEBUG("{:20} rank {:04} decode from rank {:04}", "address decode", myproc.rank, r);
        std::copy(binary_t(encoded_address.begin()), binary_t(encoded_address.end()),
            (new_locality.fabric_data_writable()));
      }
      else { new_locality = here_; }
      // insert locality into address vector
      SPDLOG_DEBUG("{:20} rank {:04} decode from rank {:04}", "insert_address", myproc.rank, r);
      new_locality = insert_address(av_, new_locality);
      if (r == 0) { root_ = new_locality; }
    }
    SPDLOG_DEBUG("{:20} on rank {:04}", "Completed boot_PMI", rank);
  }
#endif
};
