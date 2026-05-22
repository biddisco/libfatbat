#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt
import pandas as pd

RUN_RE = re.compile(
    r"^--- Run: NODES=(?P<nodes>\d+) RPN=(?P<rpn>\d+) TASKS=(?P<tasks>\d+) GPU=(?P<gpu>\d+) GPU_DEVICE=(?P<gpu_device>\d+) ---$"
)
BENCH_RE = re.compile(r"^#\s+rdma\s+(?P<bench>read|write|writedata)\s+benchmark\s*$", re.IGNORECASE)
ROW_RE = re.compile(
    r"^\s*(?P<bytes>\d+)\s+"
    r"(?P<iters>\d+)\s+"
    r"(?P<ops>\d+)\s+"
    r"(?P<time_ms>[0-9.]+)\s+"
    r"(?P<msg_rate>[0-9.]+)\s+"
    r"(?P<agg_bw>[0-9.]+)\s*$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Parse libfatbat benchmark slurm outputs and generate bandwidth bar charts."
    )
    parser.add_argument(
        "input_dir",
        nargs="?",
        default="/home/biddisco/benchmarking-results/libfatbat/2026-05-22",
        help="Directory containing slurm-lfb-*.out files",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Output directory for figures and csv summary (default: <input_dir>/plots)",
    )
    parser.add_argument(
        "--format",
        default="png",
        choices=["png", "pdf", "svg"],
        help="Figure format",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=180,
        help="Figure DPI",
    )
    return parser.parse_args()


def maybe_benchmark_from_filename(path: Path) -> Optional[str]:
    name = path.name
    for bench in ("read", "write", "writedata"):
        if f"-lfb-{bench}" in name:
            return bench
    return None


def parse_file(path: Path) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []

    current_run: Optional[Dict[str, object]] = None
    current_bench: Optional[str] = None

    for raw_line in path.read_text(errors="replace").splitlines():
        line = raw_line.strip("\n")

        m_run = RUN_RE.match(line)
        if m_run:
            current_run = {
                "nodes": int(m_run.group("nodes")),
                "rpn": int(m_run.group("rpn")),
                "tasks": int(m_run.group("tasks")),
                "gpu": int(m_run.group("gpu")),
                "gpu_device": int(m_run.group("gpu_device")),
            }
            current_bench = None
            continue

        m_bench = BENCH_RE.match(line)
        if m_bench:
            current_bench = m_bench.group("bench").lower()
            continue

        m_row = ROW_RE.match(line)
        if m_row and current_run is not None:
            bench = current_bench or maybe_benchmark_from_filename(path)
            if bench is None:
                continue

            rows.append(
                {
                    "file": path.name,
                    "benchmark": bench,
                    "nodes": int(current_run["nodes"]),
                    "rpn": int(current_run["rpn"]),
                    "tasks": int(current_run["tasks"]),
                    "gpu": int(current_run["gpu"]),
                    "gpu_device": int(current_run["gpu_device"]),
                    "bytes": int(m_row.group("bytes")),
                    "iters": int(m_row.group("iters")),
                    "ops": int(m_row.group("ops")),
                    "time_ms": float(m_row.group("time_ms")),
                    "msg_rate_mps": float(m_row.group("msg_rate")),
                    "agg_bw_mb_s": float(m_row.group("agg_bw")),
                }
            )

    return rows


def collect_rows(input_dir: Path) -> pd.DataFrame:
    files = sorted(input_dir.glob("slurm-lfb-*.out"))
    all_rows: List[Dict[str, object]] = []
    for f in files:
        all_rows.extend(parse_file(f))

    if not all_rows:
        raise RuntimeError(
            f"No benchmark table rows found in {input_dir}. "
            "Check that output files contain benchmark table sections."
        )

    return pd.DataFrame(all_rows)


def aggregate_peak(df: pd.DataFrame) -> pd.DataFrame:
    group_cols = ["benchmark", "nodes", "rpn", "gpu", "gpu_device"]
    peak = df.groupby(group_cols, as_index=False)["agg_bw_mb_s"].max()
    peak = peak.rename(columns={"agg_bw_mb_s": "peak_agg_bw_mb_s"})
    return peak


def sorted_nodes(values: pd.Series) -> List[int]:
    return sorted({int(v) for v in values})


def scale_bandwidth(values: List[float]) -> Tuple[List[float], str]:
    if not values:
        return values, "MB/s"

    max_value = max(values)
    if max_value >= 1000.0:
        return [value / 1000.0 for value in values], "GB/s"
    return values, "MB/s"


def plot_overview(
    peak: pd.DataFrame,
    gpu_flag: int,
    output_path: Path,
    fig_format: str,
    dpi: int,
) -> None:
    sub = peak[peak["gpu"] == gpu_flag].copy()
    if sub.empty:
        return

    nodes = sorted_nodes(sub["nodes"])

    combos: List[Tuple[int, str]] = [
        (rpn, bench)
        for rpn in (1, 2, 4)
        for bench in ("read", "write", "writedata")
    ]

    matrix: Dict[Tuple[int, str], List[float]] = {}
    for combo in combos:
        rpn, bench = combo
        vals: List[float] = []
        for n in nodes:
            hit = sub[(sub["nodes"] == n) & (sub["rpn"] == rpn) & (sub["benchmark"] == bench)]
            vals.append(float(hit["peak_agg_bw_mb_s"].iloc[0]) if not hit.empty else 0.0)
        matrix[combo] = vals

    all_values = [value for vals in matrix.values() for value in vals]
    _, unit = scale_bandwidth(all_values)
    if unit == "GB/s":
        for combo in combos:
            matrix[combo] = [value / 1000.0 for value in matrix[combo]]

    fig, ax = plt.subplots(figsize=(16, 7))
    x = list(range(len(nodes)))
    n_series = len(combos)
    width = 0.8 / n_series

    for i, combo in enumerate(combos):
        rpn, bench = combo
        offset = (i - (n_series - 1) / 2.0) * width
        ax.bar(
            [xi + offset for xi in x],
            matrix[combo],
            width=width,
            label=f"{rpn}rpn-{bench}",
        )

    ax.set_title("Aggregate Bandwidth vs Nodes ({})".format("GPU" if gpu_flag else "Host"))
    ax.set_xlabel("Nodes")
    ax.set_ylabel(f"Bandwidth ({unit})")
    ax.set_xticks(x)
    ax.set_xticklabels([str(n) for n in nodes])
    ax.grid(axis="y", linestyle="--", alpha=0.3)
    ax.legend(ncol=3, fontsize=9)
    fig.tight_layout()
    fig.savefig(output_path.with_suffix(f".{fig_format}"), dpi=dpi)
    plt.close(fig)


def plot_per_rpn(
    peak: pd.DataFrame,
    gpu_flag: int,
    rpn: int,
    output_path: Path,
    fig_format: str,
    dpi: int,
) -> None:
    sub = peak[(peak["gpu"] == gpu_flag) & (peak["rpn"] == rpn)].copy()
    if sub.empty:
        return

    nodes = sorted_nodes(sub["nodes"])
    benches = ["read", "write", "writedata"]

    series: Dict[str, List[float]] = {}
    for bench in benches:
        vals: List[float] = []
        for n in nodes:
            hit = sub[(sub["nodes"] == n) & (sub["benchmark"] == bench)]
            vals.append(float(hit["peak_agg_bw_mb_s"].iloc[0]) if not hit.empty else 0.0)
        series[bench] = vals

    all_values = [value for vals in series.values() for value in vals]
    _, unit = scale_bandwidth(all_values)
    if unit == "GB/s":
        for bench in benches:
            series[bench] = [value / 1000.0 for value in series[bench]]

    fig, ax = plt.subplots(figsize=(11, 6))
    x = list(range(len(nodes)))
    n_series = len(benches)
    width = 0.8 / n_series

    for i, bench in enumerate(benches):
        offset = (i - (n_series - 1) / 2.0) * width
        ax.bar([xi + offset for xi in x], series[bench], width=width, label=bench)

    ax.set_title(
        "Aggregate Bandwidth vs Nodes ({}, {} rank/node)".format(
            "GPU" if gpu_flag else "Host", rpn
        )
    )
    ax.set_xlabel("Nodes")
    ax.set_ylabel(f"Bandwidth ({unit})")
    ax.set_xticks(x)
    ax.set_xticklabels([str(n) for n in nodes])
    ax.grid(axis="y", linestyle="--", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_path.with_suffix(f".{fig_format}"), dpi=dpi)
    plt.close(fig)


def main() -> None:
    args = parse_args()
    input_dir = Path(args.input_dir).expanduser().resolve()
    output_dir = Path(args.output_dir).expanduser().resolve() if args.output_dir else input_dir / "plots"
    output_dir.mkdir(parents=True, exist_ok=True)

    raw = collect_rows(input_dir)
    peak = aggregate_peak(raw)

    raw.to_csv(output_dir / "raw_rows.csv", index=False)
    peak.to_csv(output_dir / "peak_bandwidth.csv", index=False)

    plot_overview(peak, gpu_flag=0, output_path=output_dir / "bandwidth_host_overview", fig_format=args.format, dpi=args.dpi)
    plot_overview(peak, gpu_flag=1, output_path=output_dir / "bandwidth_gpu_overview", fig_format=args.format, dpi=args.dpi)

    for gpu_flag in (0, 1):
        mode = "gpu" if gpu_flag else "host"
        for rpn in (1, 2, 4):
            plot_per_rpn(
                peak,
                gpu_flag=gpu_flag,
                rpn=rpn,
                output_path=output_dir / f"bandwidth_{mode}_rpn{rpn}",
                fig_format=args.format,
                dpi=args.dpi,
            )

    print(f"Parsed rows: {len(raw)}")
    print(f"Peak rows:   {len(peak)}")
    print(f"Output dir:  {output_dir}")


if __name__ == "__main__":
    main()
