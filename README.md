
# libfatbat (libfabric transport abstractions) 
 
## Table of Contents
- [libfatbat (libfabric transport abstractions)](#libfatbat-libfabric-transport-abstractions)
  - [Table of Contents](#table-of-contents)
  - [Introduction/Description](#introductiondescription)
  - [Provider setup](#provider-setup)
  - [PMI dependency (obtained via MPI)](#pmi-dependency-obtained-via-mpi)
  - [Building/Testing on laptop (tcp provider)](#buildingtesting-on-laptop-tcp-provider)
  - [Building/Testing on alps (CSCS) : uenv for cray-mpich : tcp provider](#buildingtesting-on-alps-cscs--uenv-for-cray-mpich--tcp-provider)
  - [Building/Testing on alps (CSCS) : uenv for cray-mpich : cxi provider](#buildingtesting-on-alps-cscs--uenv-for-cray-mpich--cxi-provider)
  - [Building/Testing on alps (CSCS) : uenv for OpenMPI : cxi provider](#buildingtesting-on-alps-cscs--uenv-for-openmpi--cxi-provider)

<p align="center"> 
<img src="./images/fatbat-transparent.png" alt="drawing" width="256"/>
</p>

## Introduction/Description
**What is libfatbat**: Some libfabric utilites collected into a simple library.

**Why create it**: After working on several C++ projects that use libfabric, some pieces of code to setup connections, endpoints, memory pinning, poll completion queues and dispatch completion handlers etc etc were duplicated and the reusable parts can be made into a simple library with some options to control common choices. The library consists of a few headers with a *controller* class that can be subclassed on a per-project basis - the *controller* exposes features that can be lightly customized as needed.

<a id="provider-setup"></a>
## Provider setup
The correct way to use libfabric is to query the library to find providers, and their supported features, and then adjust your code paths to enable/disable functionality based on what is available in libfabric on your system.
Since inital work at CSCS was dedicated to the Cray XC50 with Gemini interconnect (`gni` provider), and another machine with infiniband (`verbs` provider) the precursor to libfatbat was written with a couple of `#ifdefs` in place that allowed you, at compile time, (via cmake) to select the provider and then the features we needed were hardcoded in and it either ran ok, or it didn't.
As new machines were used, the number of provider related `#ifdefs` grew slightly, but since we only ran tests on machines that we knew supported the features we needed, the basic idea of selecting the provider at compile time and then building code specifically for that machine remains in place.
If you need RMA features for example, then you just can't run on a machine that doesn't support `FI_RMA` and so this can be hardcoded in the controller flags and there is no need to complicate the code with checks to see if we can do this or that and enable or disable things at run time (or emulate the features that are not supported - the `rxm/rxd` providers help with this aspect). Either it compiles and runs, or it doesn't and you find a machine that supports the features you need (with some minor exceptions where we do support checks).
The reader is directed to the libfabric [Feature matrix](https://github.com/ofiwg/libfabric/wiki/Provider-Feature-Matrix-main) to get an idea of what is/isn't avaialble. 

NB. When you select provider `FATBAT_PROVIDER=tcp` in the cmake options, internally, this is substituted with `tcp;ofi_rxm` because (if you consult the feature matrix), you will see that tcp alone, does not support features like `FI_TAGGED` and using rxm layed on tcp allows us to use it. We do the same for verbs, though that provider has not been tested for a while and it would be a good idea to check if all runs ok.

<a id="pmi-dependency"></a>
## PMI dependency (obtained via MPI)
libfatbat does not need or use MPI, however, launching `n` processes on `N` nodes is simplified by using standard MPI launch tools that are integrated with job controllers like slurm, and so, by default, we expect there to be a PMI instance available [(see Process Management Interface library OpenPMIx for details)](https://github.com/openpmix/openpmix), which is usually setup by slurm `srun` or `mpiexec`/`mpirun` when launching a multi rank job. For this reason, the setup and testing instructions below assume the presence of MPI which we use to find PMI (strictly PMI2 or PMIx) for job initialization.

At CSCS we are using spack to manage packages, so the following instructions make use of spack commands and environments and uenvs (CSCS alps machines), if you are not using spack, you shoudd replace the spack related setup with your own package/module/other commands. 

The following instructions assume libfatbat is checked out in $HOME/src/libfatbat - please adjust commands accordingly to suit your location.
```
git clone https://github.com/biddisco/libfatbat ~/src/libfatbat 
```

<a id="building-testing-laptop-tcp"></a>
## Building/Testing on laptop (tcp provider)
For development purposes, testing on a laptop / local machine is convenient. If a package manager such as [spack](https://github.com/spack/spack) is used, then an environment containing OpenMPI is a good starting point as it provides `mpiexec` which we can use for launching processes. The following spec snippet is what I am using on my laptop. (`cuda_arch` should be changed appropriately if you have an NVidia GPU, otherwise omit `+cuda cuda_arch=...`). The MPI details are not important, but you might as well build a version you can use for other projects too. 
NB. Dependencies: Also needed are `spdlog` and `fmt` if loglevel is not set to `OFF`. Note also that we set CMAKE_CUDA_ARCHITECTURE just because we will be testing GPU buffer transfers at some point and our cuda allocator (hwmalloc) 
``` 
  # install openmpi, pmix and libfabric, with recent (2025/12/18) versions
  - openmpi@5.0.9 fabrics=ofi ~internal-pmix +cuda cuda_arch:=89
  - pmix
  - libfabric@2.3:
  - spdlog 
  - fmt
```
Assuming you have a spack environment (as above) containing openmpi and pmi (mpich should work too, but I'm using openmpi for this demo)
```
# if using spack for package management
spack env activate <your-spack-env-here>
```
Compile libfatbat (actually, just the tests as the main stuff is header-only),
```
# insert your own build location
cd ~/build/libfatbat

# enable logging (level=trace)
# set libfabric provider to tcp
# enable hwmalloc device memory allocator 
cmake -DCMAKE_CUDA_ARCHITECTURES=89 \
    -DCMAKE_BUILD_TYPE=Debug        \
    -DFATBAT_PROVIDER=tcp           \
    -DFATBAT_LOG_LEVEL=trace        \
    -DHWMALLOC_ENABLE_DEVICE=ON     \
    ~/src/libfatbat/
```
after building, try running ctest to see if examples generate output 
```
ctest -V
```

<a id="building-testing-alps-cray-mpich-tcp"></a>
## Building/Testing on alps (CSCS) : uenv for cray-mpich : tcp provider
We don't really want tcp, but for first testing, this is a useful fallback.
The cray-mpich based uenvs use cray-pmi which by default is not loaded in the view, so we must manually do so to make sure cmake finds it ok
```
# Load a cray-mpich based uenv (this one has all the other tools we need)
uenv start --view=default \
  /capstor/store/cscs/cscs/public/uenvs/opal-x-gh200-mpich-gcc-2025-12-15.squashfs

# make sure cray-pmi (supplied in the uenv image) is findable
spack load cray-pmi
```
```
# compile libfatbat,
# enable logging (level=trace)
# set libfabric provider to tcp for #1 test
# enable hwmalloc device memory allocator 
cmake -DCMAKE_CUDA_ARCHITECTURES=90 \
    -DCMAKE_BUILD_TYPE=Debug        \
    -DFATBAT_PROVIDER=tcp           \
    -DFATBAT_LOG_LEVEL=trace        \
    -DHWMALLOC_ENABLE_DEVICE=ON     \
    ~/src/libfatbat/
```
and then after building try running ctest to see if examples generate output (You will need a compute node or two)
```
# allocate debug partition 2 nodes
salloc -N 2 --time=00:30:00 -p debug
ctest -V
```
You should see log info and a successful set of examples

**Warning**: If you see this in the cmake output (no `PC_PMI_CRAY_FOUND` and instead pmix version xxxxx)
```
-- PMI: PkgConfig PC_PMI_CRAY_FOUND is 
-- PMI: PkgConfig PC_PMI_FOUND is 
-- Checking for module 'pmix'
--   Found pmix, version 5.0.3
```
then when you run a test you get 
```
PMIx_ PMIx Init failed: PMIX_ERR_UNREACH
terminate called after throwing an instance of 'std::runtime_error'
  what():  PMIx failure
```
The you have picked up the default system pmi and probably forgot to `spack load cray-pmi` - **Fix**: load cray-pmi, wipe your cmake cache and rerun

<a id="building-testing-alps-cray-mpich-cxi"></a>
## Building/Testing on alps (CSCS) : uenv for cray-mpich : cxi provider
The procedure is the same as the above one for tcp, except we set provider to `cxi`
```
cmake -DCMAKE_CUDA_ARCHITECTURES=90 \
    -DCMAKE_BUILD_TYPE=Debug        \
    -DFATBAT_PROVIDER=cxi           \
    -DFATBAT_LOG_LEVEL=trace        \
    -DHWMALLOC_ENABLE_DEVICE=ON     \
    ~/src/libfatbat/
```
Test should show cxi prominently in the log output

<a id="building-testing-alps-openmpi-cxi"></a>
## Building/Testing on alps (CSCS) : uenv for OpenMPI : cxi provider
Load an OpenMPI based uenv as follows and then run cmake, using the `cxi` provider. There is no need to load any pmi libraery as we have asked for it in the OpenMPI uenv as a top level library, so it is part of the default view. 
```
uenv start --view=default \
  /capstor/store/cscs/cscs/public/uenvs/opal-x-gh200-ompi-gcc-2025-12-15.squashfs
...
cmake -DCMAKE_CUDA_ARCHITECTURES=90 \
    -DCMAKE_BUILD_TYPE=Debug        \
    -DFATBAT_PROVIDER=cxi           \
    -DFATBAT_LOG_LEVEL=trace        \
    -DHWMALLOC_ENABLE_DEVICE=ON     \
    ~/src/libfatbat/
```


