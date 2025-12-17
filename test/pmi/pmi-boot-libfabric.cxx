//
#include "../test_controller.hpp"
#include "libfatbat/logging.hpp"

// ----------------------------------------------------------------------------
#ifdef FATBAT_PMI2_ENABLED
int main(int argx, char** argv)
{
# if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::trace);
# endif

  int spawned;
  int appnum;
  int rank = -1;
  int node = -1;
  int size = -1;

  {
    SPDLOG_SCOPE("PMI init");
    PMI2_Init(&spawned, &size, &rank, &appnum);
  }
  test_controller controller;
  {
    SPDLOG_SCOPE("controller.initialize");
    controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, 1);
  }
  {
    SPDLOG_SCOPE("controller.boot_PMI");
    controller.boot_PMI(rank, size);
    controller.debug_print_av_vector(size);
  }
}
#endif

// ----------------------------------------------------------------------------
#ifdef FATBAT_PMIx_ENABLED
static pmix_proc_t myproc;
pmix_status_t rc;
pmix_proc_t* procs;
size_t nprocs, n;
pid_t pid;
char* nodelist;
pmix_nspace_t nspace;

pmix_value_t value;
char* tmp;

int main(int argc, char** argv)
{
# if defined(SPDLOG_ACTIVE_LEVEL) && (SPDLOG_ACTIVE_LEVEL != SPDLOG_LEVEL_OFF)
  spdlog::set_pattern("[%^%-8l%$]%t| %v");
  spdlog::set_level(spdlog::level::trace);
# endif

  pmix_proc_t myproc;    // local process info
  pmix_proc_t proc;      // other process info
  pmix_value_t* val = NULL;
  int rank = -1;
  int node = -1;
  int size = -1;

  // init pmix
  CHECK_PMIX("PMIx Init", PMIx_Init(&myproc, NULL, 0));

  // copy myproc details into struct
  PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_INVALID);

  // get our rank
  CHECK_PMIX("Get Rank", PMIx_Get(&proc, PMIX_RANK, NULL, 0, &val));
  rank = val->data.uint32;
  PMIX_VALUE_RELEASE(val);

  if (rank == 0)
  {
    // std::cout << "Please attach debugger and hit return" << std::endl;
    // char c;
    // std::cin >> c;
  }

  // get our node ID
  CHECK_PMIX("Get Node", PMIx_Get(&myproc, PMIX_NODEID, NULL, 0, &val));
  node = val->data.uint32;
  PMIX_VALUE_RELEASE(val);

  // get size
  CHECK_PMIX("Get Size", PMIx_Get(&proc, PMIX_APP_SIZE, NULL, 0, &val));
  size = val->data.uint32;
  PMIX_VALUE_RELEASE(val);

  SPDLOG_DEBUG("{:30}: Rank {}/{} : on Node {}", PMIx_Proc_string(&myproc), rank, size, node);

  test_controller controller;

  {
    SPDLOG_SCOPE("controller.initialize");
    controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, 1);
  }
  {
    SPDLOG_SCOPE("controller.boot_PMI");
    controller.boot_PMI(myproc, rank, size);
    controller.debug_print_av_vector(size);
  }

  // finalize us
  CHECK_PMIX("PMIx Finalize", PMIx_Finalize(NULL, 0));
  return 0;
}
#endif

/*
// --------------------------------------------------------------------
// All nodes can share their addresses using this function
void boot_PMI()
{
  int size;
  int rank;

  LF_DEB(NS_DEBUG::cnt_boot, debug(debug::str<>("Calling PMI init")));

  if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0)))
  {
    fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
        PMIx_Error_string(rc));
    exit(0);
  }
  fprintf(stderr, "Client ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank,
      (unsigned long) pid);

  using namespace libfatbat;
  libfatbat::locality here_;

  // we must pass our libfabric data to other nodes
  // encode it as a string to put into the PMI KV store
  std::string encoded_locality(base64_t((char const*) (&here_.fabric_data())),
      base64_t((char const*) (&here_.fabric_data()) + locality_defs::array_size));

  // std::string encoded_locality(base64_t((char const*) (fabric_data.data())),
  //     base64_t((char const*) (fabric_data.data()) + locality_defs::array_size));
  int encoded_length = encoded_locality.size();
  LF_DEB(NS_DEBUG::cnt_boot,
      debug(debug::str<>("Encoded locality as"), encoded_locality, "with length ",
          debug::dec<>(encoded_length)));

  // Key name for PMI
  std::string pmi_key = "LIBFABRIC_" + std::to_string(rank);
  // insert our data in the KV store
  LF_DEB(NS_DEBUG::cnt_boot, debug(debug::str<>("PMI2_KVS_Put"), "on rank", debug::dec<>(rank)));
  PMIx_Put(pmi_key.data(), encoded_locality.data());

  // Wait for all to do the same
  LF_DEB(NS_DEBUG::cnt_boot, debug(debug::str<>("PMI2_KVS_Fence"), "on rank", debug::dec<>(rank)));
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
        NS_DEBUG::cnt_deb.error("PMI value length mismatch, expected ",
            debug::dec<>(encoded_length), "got ", debug::dec<>(length));
      }
      // decode the string back to raw locality data
      LF_DEB(NS_DEBUG::cnt_deb,
          trace(
              "Calling decode for ", debug::dec<>(i), " locality data on ran", debug::dec<>(rank)));
      std::copy(binary_t(encoded_data), binary_t(encoded_data + encoded_length),
          (new_locality.fabric_data_writable()));
    }
    else { new_locality = here_; }

    // insert locality into address vector
    LF_DEB(NS_DEBUG::cnt_deb,
        trace("Calling insert_address for", debug::dec<>(i), "on rank", debug::dec<>(rank)));
    new_locality = insert_address(av_, new_locality);
    if (i == 0) { root_ = new_locality; }
  }

  PMI2_Finalize();
  LF_DEB(NS_DEBUG::cnt_boot, debug("Completed PMI finalize on rank", debug::dec<>(rank)));
}
*/

// int main(int argc, char** argv)
// {
//   pmix_status_t rc;
//   pmix_proc_t* procs;
//   size_t nprocs, n;
//   pid_t pid;
//   char* nodelist;
//   pmix_nspace_t nspace;

//   pid = getpid();
//   fprintf(stderr, "Client %lu: Running\n", (unsigned long) pid);

//   if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0)))
//   {
//     fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
//         PMIx_Error_string(rc));
//     exit(0);
//   }
//   fprintf(stderr, "Client ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank,
//       (unsigned long) pid);

//   rc = PMIx_Resolve_peers(NULL, myproc.nspace, &procs, &nprocs);
//   fprintf(stderr, "ResPeers returned: %s\n", PMIx_Error_string(rc));
//   if (PMIX_SUCCESS == rc)
//   {
//     for (n = 0; n < nprocs; n++) { fprintf(stderr, "\t%s:%u\n", procs[n].nspace, procs[n].rank); }
//   }

//   rc = PMIx_Resolve_nodes(myproc.nspace, &nodelist);
//   fprintf(stderr, "ResNodes returned: %s\n", PMIx_Error_string(rc));
//   if (PMIX_SUCCESS == rc) { fprintf(stderr, "\t%s\n", nodelist); }

//   // now do global request
//   PMIX_LOAD_NSPACE(nspace, NULL);
//   rc = PMIx_Resolve_peers(NULL, nspace, &procs, &nprocs);
//   fprintf(stderr, "ResPeers global returned: %s\n", PMIx_Error_string(rc));
//   if (PMIX_SUCCESS == rc)
//   {
//     for (n = 0; n < nprocs; n++) { fprintf(stderr, "\t%s:%u\n", procs[n].nspace, procs[n].rank); }
//   }

//   rc = PMIx_Resolve_nodes(nspace, &nodelist);
//   fprintf(stderr, "ResNodes global returned: %s\n", PMIx_Error_string(rc));
//   if (PMIX_SUCCESS == rc) { fprintf(stderr, "\t%s\n", nodelist); }

//   /* finalize us */
//   rc = PMIx_Finalize(NULL, 0);
//   fflush(stderr);
//   return (rc);
// }

// // int main(int argc, char** argv)
// // {
// //   boot_PMI();
// //   return 0;
// // }
