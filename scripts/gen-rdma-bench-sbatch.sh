#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
Generate one Slurm batch script per node count for libfatbat RDMA benchmarks.

The generated scripts run an inner loop over ranks-per-node: 1, 2, 4.
Node counts are generated as: 1, 2, 4, ... up to max-nodes, and include max-nodes
as a final point when max-nodes is not an exact power of two.

Usage:
  gen-rdma-bench-sbatch.sh --max-nodes M --benchmark read|write|writedata [options]

Required:
  --max-nodes M              Maximum node count to generate scripts for (M >= 1)
  --benchmark MODE           Benchmark mode: read, write, or writedata

Optional:
  --iterations I             Iterations passed to benchmark via -i (default: 10000)
  --gpu                      Pass --gpu to the benchmark and tag outputs as GPU runs
  --gpu-device ID            GPU device id passed with --gpu-device (default: 0)
  --wrapper PATH             Wrapper script path (default: wrapper-mpi-binding.sh)
  --bin-dir PATH             Directory containing benchmark binaries
                             (default: /capstor/scratch/cscs/biddisco/build-daint/libfatbat/bin)
  --out-dir PATH             Output directory for generated .sbatch files
                             (default: scripts/generated-bench)
  --time HH:MM:SS            Slurm walltime (default: 00:20:00)
  --partition NAME           Slurm partition (optional)
  --account NAME             Slurm account/project (optional)
  --extra-args "ARGS"        Extra benchmark args appended after -i ITER
  --help                     Show this help

Examples:
  ./scripts/gen-rdma-bench-sbatch.sh --max-nodes 16 --benchmark writedata
  ./scripts/gen-rdma-bench-sbatch.sh --max-nodes 8 --benchmark write \
    --partition debug --time 00:30:00 --iterations 20000
USAGE
}

MAX_NODES=""
BENCHMARK=""
ITERATIONS="10000"
WRAPPER="wrapper-mpi-binding.sh"
BIN_DIR="/capstor/scratch/cscs/biddisco/build-daint/libfatbat/bin"
OUT_DIR="scripts/generated-bench"
SBATCH_TIME="00:20:00"
SBATCH_PARTITION=""
SBATCH_ACCOUNT=""
EXTRA_ARGS=""
GPU_MODE=0
GPU_DEVICE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --max-nodes)
      MAX_NODES="${2:-}"
      shift 2
      ;;
    --benchmark)
      BENCHMARK="${2:-}"
      shift 2
      ;;
    --iterations)
      ITERATIONS="${2:-}"
      shift 2
      ;;
    --gpu)
      GPU_MODE=1
      shift
      ;;
    --gpu-device)
      GPU_DEVICE="${2:-}"
      shift 2
      ;;
    --wrapper)
      WRAPPER="${2:-}"
      shift 2
      ;;
    --bin-dir)
      BIN_DIR="${2:-}"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="${2:-}"
      shift 2
      ;;
    --time)
      SBATCH_TIME="${2:-}"
      shift 2
      ;;
    --partition)
      SBATCH_PARTITION="${2:-}"
      shift 2
      ;;
    --account)
      SBATCH_ACCOUNT="${2:-}"
      shift 2
      ;;
    --extra-args)
      EXTRA_ARGS="${2:-}"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$MAX_NODES" || -z "$BENCHMARK" ]]; then
  echo "Error: --max-nodes and --benchmark are required." >&2
  usage >&2
  exit 1
fi

if ! [[ "$MAX_NODES" =~ ^[0-9]+$ ]] || [[ "$MAX_NODES" -lt 1 ]]; then
  echo "Error: --max-nodes must be an integer >= 1." >&2
  exit 1
fi

if ! [[ "$ITERATIONS" =~ ^[0-9]+$ ]] || [[ "$ITERATIONS" -lt 1 ]]; then
  echo "Error: --iterations must be an integer >= 1." >&2
  exit 1
fi

if ! [[ "$GPU_DEVICE" =~ ^[0-9]+$ ]]; then
  echo "Error: --gpu-device must be an integer >= 0." >&2
  exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ "$WRAPPER" != /* ]]; then
  WRAPPER="${SCRIPT_DIR}/${WRAPPER}"
fi

case "$BENCHMARK" in
  read)
    BENCH_BIN_NAME="rdma-read-benchmark"
    ;;
  write)
    BENCH_BIN_NAME="rdma-write-benchmark"
    ;;
  writedata)
    BENCH_BIN_NAME="rdma-writedata-benchmark"
    ;;
  *)
    echo "Error: --benchmark must be one of: read, write, writedata." >&2
    exit 1
    ;;
esac

mkdir -p "$OUT_DIR"

run_suffix=""
if [[ "$GPU_MODE" -eq 1 ]]; then
  run_suffix="-gpu"
fi

generated_files=()

node_counts=()
n=1
while [[ "$n" -le "$MAX_NODES" ]]; do
  node_counts+=("$n")
  n=$(( n * 2 ))
done
if [[ "${node_counts[-1]}" -ne "$MAX_NODES" ]]; then
  node_counts+=("$MAX_NODES")
fi

for nodes in "${node_counts[@]}"; do
  outfile="${OUT_DIR}/bench-${BENCHMARK}${run_suffix}-N${nodes}.sbatch"

  {
    echo "#!/usr/bin/env bash"
    echo "#SBATCH --job-name=lfb-${BENCHMARK}${run_suffix}-N${nodes}"
    echo "#SBATCH --nodes=${nodes}"
    echo "#SBATCH --time=${SBATCH_TIME}"
    echo "#SBATCH --output=slurm-%x-%j.out"
    if [[ -n "$SBATCH_PARTITION" ]]; then
      echo "#SBATCH --partition=${SBATCH_PARTITION}"
    fi
    if [[ -n "$SBATCH_ACCOUNT" ]]; then
      echo "#SBATCH --account=${SBATCH_ACCOUNT}"
    fi

    cat <<SCRIPT_BODY

set -euo pipefail

export IPC_ON=0
export FI_CXI_ENABLE_WRITEDATA=1

NODES=${nodes}
ITERATIONS=${ITERATIONS}
WRAPPER="${WRAPPER}"
BENCH_BIN="${BIN_DIR}/${BENCH_BIN_NAME}"
EXTRA_ARGS="${EXTRA_ARGS}"
GPU_MODE=${GPU_MODE}
GPU_DEVICE=${GPU_DEVICE}

if [[ ! -x "\${WRAPPER}" ]]; then
  echo "Wrapper not executable: \${WRAPPER}" >&2
  exit 1
fi

if [[ ! -x "\${BENCH_BIN}" ]]; then
  echo "Benchmark binary not executable: \${BENCH_BIN}" >&2
  exit 1
fi

echo "=== Benchmark start: benchmark=${BENCHMARK} nodes=\${NODES} ==="
for RPN in 4 2 1; do
  TASKS=\$(( NODES * RPN ))
  echo "--- Run: NODES=\${NODES} RPN=\${RPN} TASKS=\${TASKS} GPU=\${GPU_MODE} GPU_DEVICE=\${GPU_DEVICE} ---"

  if [[ -n "\${EXTRA_ARGS}" ]]; then
    # shellcheck disable=SC2206
    EXTRA=(\${EXTRA_ARGS})
  else
    EXTRA=()
  fi

  GPU_ARGS=()
  if [[ "\${GPU_MODE}" -eq 1 ]]; then
    GPU_ARGS=(--gpu --gpu-device "\${GPU_DEVICE}")
  fi

  srun -N "\${NODES}" -n "\${TASKS}" "\${WRAPPER}" "\${BENCH_BIN}" -i "\${ITERATIONS}" "\${GPU_ARGS[@]}" "\${EXTRA[@]}"
done
echo "=== Benchmark end: benchmark=${BENCHMARK} nodes=\${NODES} ==="
SCRIPT_BODY
  } > "$outfile"

  chmod +x "$outfile"
  echo "Generated $outfile"
  abs_outfile=$(cd "$(dirname "$outfile")" && pwd)/"$(basename "$outfile")"
  generated_files+=("$abs_outfile")
done

timestamp=$(date +%Y%m%d-%H%M%S)
submit_script="$PWD/submit-bench-${BENCHMARK}${run_suffix}-${timestamp}.sh"
{
  echo "#!/usr/bin/env bash"
  echo "set -euo pipefail"
  echo ""
  echo "# Auto-generated by gen-rdma-bench-sbatch.sh"
  echo "# Generated at: $(date -Is)"
  echo ""
  for generated in "${generated_files[@]}"; do
    echo "sbatch \"$generated\""
  done
} > "$submit_script"
chmod +x "$submit_script"

echo ""
echo "Done. Submit with:"
echo "  $submit_script"
