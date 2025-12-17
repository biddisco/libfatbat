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
    SPDLOG_SCOPE("{}","PMI init");
    PMI2_Init(&spawned, &size, &rank, &appnum);
  }
  test_controller controller;
  {
    SPDLOG_SCOPE("{}", "controller.initialize");
    controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, 1);
  }
  {
    SPDLOG_SCOPE("{}", "controller.boot_PMI");
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
    SPDLOG_SCOPE("{}", "controller.initialize");
    controller.initialize(HAVE_LIBFATBAT_PROVIDER, rank == 0, size, 1);
  }
  {
    SPDLOG_SCOPE("{}", "controller.boot_PMI");
    controller.boot_PMI(myproc, rank, size);
    controller.debug_print_av_vector(size);
  }

  // finalize us
  CHECK_PMIX("PMIx Finalize", PMIx_Finalize(NULL, 0));
  return 0;
}
#endif
