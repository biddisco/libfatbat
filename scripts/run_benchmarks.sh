#!/bin/bash

MAX_NODES=16

for bench in read write writedata; do

  echo "Generating $bench"
  /users/biddisco/src/libfatbat/scripts/gen-rdma-bench-sbatch.sh --max-nodes $MAX_NODES --benchmark $bench --account csstaff --out-dir ./$bench
  find ./$bench -name bench-$bench-N* | xargs -n 1 sbatch
  
  echo "Generating $bench-gpu"
  /users/biddisco/src/libfatbat/scripts/gen-rdma-bench-sbatch.sh --max-nodes $MAX_NODES --benchmark $bench --gpu --account csstaff --out-dir ./$bench-gpu 
  find ./$bench-gpu -name bench-$bench-gpu-N\* | xargs -n 1 sbatch
done

