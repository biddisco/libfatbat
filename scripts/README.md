# Scripts Quick Reference

This folder contains helper scripts for generating RDMA benchmark Slurm jobs and plotting results.

## Plot Benchmark Results

Script: plot_benchmark_results.py

Parses slurm-lfb-*.out files and writes CSV summaries + plots.

### Common usage

```bash
python /home/biddisco/src/libfatbat/scripts/plot_benchmark_results.py \
  ~/benchmarking-results/libfatbat/2026-05-22/clariden \
  --output-dir ~/benchmarking-results/libfatbat/2026-05-22/clariden/plots
```

### Mode: peak (default)

Peak bandwidth per configuration, with bar annotations for best message size.

```bash
python /home/biddisco/src/libfatbat/scripts/plot_benchmark_results.py \
  ~/benchmarking-results/libfatbat/2026-05-22/clariden \
  --mode peak
```

### Mode: fixed message size

Bandwidth for one message size only.

Notes:
- Message-size suffixes are binary-only: K, M, G, T.
- Example: 1M means 1MiB.

```bash
python /home/biddisco/src/libfatbat/scripts/plot_benchmark_results.py \
  ~/benchmarking-results/libfatbat/2026-05-22/clariden \
  --mode size --message-size 4M
```

Equivalent forms:
- --message-size 4194304
- --message-size 4096K
- --message-size 4MiB

### Mode: message-size curves

Plots bandwidth vs message size (log2 x-axis), split by host/gpu, benchmark, and ranks-per-node.

```bash
python /home/biddisco/src/libfatbat/scripts/plot_benchmark_results.py \
  ~/benchmarking-results/libfatbat/2026-05-22/clariden \
  --mode curve
```

### Helpful options

```bash
# Save as PDF
python /home/biddisco/src/libfatbat/scripts/plot_benchmark_results.py \
  ~/benchmarking-results/libfatbat/2026-05-22/clariden \
  --mode peak --format pdf

# Increase resolution
python /home/biddisco/src/libfatbat/scripts/plot_benchmark_results.py \
  ~/benchmarking-results/libfatbat/2026-05-22/clariden \
  --mode peak --dpi 250
```

## Generate RDMA Benchmark Slurm Jobs

Script: gen-rdma-bench-sbatch.sh

Creates one .sbatch script per node count, with inner loops over ranks-per-node.

### Basic usage

```bash
/home/biddisco/src/libfatbat/scripts/gen-rdma-bench-sbatch.sh \
  --max-nodes 16 \
  --benchmark writedata
```

### Typical host run

```bash
/home/biddisco/src/libfatbat/scripts/gen-rdma-bench-sbatch.sh \
  --max-nodes 8 \
  --benchmark write \
  --iterations 20000 \
  --partition debug \
  --time 00:30:00
```

### Typical GPU run

```bash
/home/biddisco/src/libfatbat/scripts/gen-rdma-bench-sbatch.sh \
  --max-nodes 8 \
  --benchmark read \
  --gpu --gpu-device 0
```

### Submit generated jobs

Each generator run now creates a run-specific submit script in your current working directory:

submit-bench-<benchmark><-gpu>-<timestamp>.sh

This script contains one explicit sbatch command per generated file from that run only.

```bash
# Run the path printed by the generator, for example:
./submit-bench-write-20260522-143500.sh
```

### Notes

- The generator default wrapper is wrapper-mpi-binding.sh.
- Relative wrapper paths are resolved against the generator script directory.
- Generated jobs fail fast if the wrapper or benchmark binary is not executable.
