#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
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


def parse_message_size_arg(value: str) -> int:
    token = value.strip().lower()
    m = re.match(r"^(\d+)\s*([a-z]*)$", token)
    if not m:
        raise argparse.ArgumentTypeError(
            f"invalid message size '{value}' (examples: 1M, 2M, 4M, 1024K, 4194304)"
        )

    count = int(m.group(1))
    suffix = m.group(2)

    multipliers = {
        "": 1,
        "b": 1,
        "k": 1024,
        "m": 1024**2,
        "g": 1024**3,
        "t": 1024**4,
        "ki": 1024,
        "kib": 1024,
        "mi": 1024**2,
        "mib": 1024**2,
        "gi": 1024**3,
        "gib": 1024**3,
        "ti": 1024**4,
        "tib": 1024**4,
    }

    if suffix not in multipliers:
        raise argparse.ArgumentTypeError(
            f"unknown size suffix '{suffix}' in '{value}' (use binary units like K/M/G/T or KiB/MiB/GiB/TiB)"
        )

    size_bytes = count * multipliers[suffix]
    if size_bytes < 0:
        raise argparse.ArgumentTypeError("message size must be >= 0")
    return size_bytes


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
    parser.add_argument(
        "--mode",
        default="peak",
        choices=["peak", "size", "curve"],
        help="Plot mode: peak bandwidth, bandwidth for a fixed message size, or full message-size curves",
    )
    parser.add_argument(
        "--message-size",
        type=parse_message_size_arg,
        default=None,
        help="Message size for --mode size using binary units (examples: 1M, 2M, 4M, 1024K, 4194304)",
    )
    return parser.parse_args()


def maybe_benchmark_from_filename(path: Path) -> Optional[str]:
    name = path.name
    for bench in ("read", "write", "writedata"):
        if f"-lfb-{bench}" in name:
            return bench
    return None


def debug_io(message: str) -> None:
    print(f"[plot-io] {message}", file=sys.stderr)


def parse_file(path: Path) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []

    current_run: Optional[Dict[str, object]] = None
    current_bench: Optional[str] = None

    debug_io(f"reading {path}")

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
    debug_io(f"scanning {input_dir} for slurm-lfb-*.out")
    all_rows: List[Dict[str, object]] = []
    skipped_files: List[Path] = []
    for f in files:
        rows = parse_file(f)
        if rows:
            all_rows.extend(rows)
        else:
            skipped_files.append(f)

    for f in skipped_files:
        print(
            f"Warning: skipping {f.name} because it does not contain any parsed benchmark rows.",
            file=sys.stderr,
        )

    if not all_rows:
        print(
            f"Warning: no benchmark table rows found in {input_dir}; plots will not be generated.",
            file=sys.stderr,
        )
        return pd.DataFrame(
            columns=[
                "file",
                "benchmark",
                "nodes",
                "rpn",
                "tasks",
                "gpu",
                "gpu_device",
                "bytes",
                "iters",
                "ops",
                "time_ms",
                "msg_rate_mps",
                "agg_bw_mb_s",
            ]
        )

    return pd.DataFrame(all_rows)


def format_bytes(value: int) -> str:
    units = ["B", "KiB", "MiB", "GiB"]
    scaled = float(value)
    for unit in units:
        if scaled < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(scaled)} {unit}"
            if scaled.is_integer():
                return f"{int(scaled)} {unit}"
            return f"{scaled:.1f} {unit}"
        scaled /= 1024.0
    return f"{value} B"


def aggregate_peak(df: pd.DataFrame) -> pd.DataFrame:
    if df.empty:
        return pd.DataFrame(
            columns=[
                "benchmark",
                "nodes",
                "rpn",
                "gpu",
                "gpu_device",
                "bytes",
                "peak_agg_bw_mb_s",
            ]
        )

    group_cols = ["benchmark", "nodes", "rpn", "gpu", "gpu_device"]
    peak_idx = df.groupby(group_cols)["agg_bw_mb_s"].idxmax()
    peak = df.loc[peak_idx, group_cols + ["bytes", "agg_bw_mb_s"]].reset_index(drop=True)
    peak = peak.rename(columns={"agg_bw_mb_s": "peak_agg_bw_mb_s"})
    return peak


def aggregate_for_message_size(df: pd.DataFrame, message_size: int) -> pd.DataFrame:
    filtered = df[df["bytes"] == message_size].copy()
    if filtered.empty:
        return pd.DataFrame(
            columns=[
                "benchmark",
                "nodes",
                "rpn",
                "gpu",
                "gpu_device",
                "bytes",
                "fixed_agg_bw_mb_s",
            ]
        )

    group_cols = ["benchmark", "nodes", "rpn", "gpu", "gpu_device", "bytes"]
    fixed = filtered.groupby(group_cols, as_index=False)["agg_bw_mb_s"].max()
    fixed = fixed.rename(columns={"agg_bw_mb_s": "fixed_agg_bw_mb_s"})
    return fixed


def sorted_nodes(values: pd.Series) -> List[int]:
    return sorted({int(v) for v in values})


def scale_bandwidth(values: List[float]) -> Tuple[List[float], str]:
    if not values:
        return values, "MB/s"

    max_value = max(values)
    if max_value >= 1000.0:
        return [value / 1000.0 for value in values], "GB/s"
    return values, "MB/s"


def annotate_bars(ax: plt.Axes, bars, labels: List[str]) -> None:
    for bar, label in zip(bars, labels):
        height = bar.get_height()
        if height <= 0.0:
            continue
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            height,
            label,
            rotation=90,
            ha="center",
            va="bottom",
            fontsize=6,
        )


def plot_overview(
    data: pd.DataFrame,
    gpu_flag: int,
    output_path: Path,
    fig_format: str,
    dpi: int,
    value_column: str,
    title_suffix: str,
    annotate_sizes: bool,
) -> None:
    sub = data[data["gpu"] == gpu_flag].copy()
    if sub.empty:
        debug_io(f"skipping plot {output_path.with_suffix(f'.{fig_format}')} because there is no data")
        return

    nodes = sorted_nodes(sub["nodes"])

    combos: List[Tuple[int, str]] = [
        (rpn, bench)
        for rpn in (1, 2, 4)
        for bench in ("read", "write", "writedata")
    ]

    matrix: Dict[Tuple[int, str], List[float]] = {}
    labels_by_combo: Dict[Tuple[int, str], List[str]] = {}
    for combo in combos:
        rpn, bench = combo
        vals: List[float] = []
        labels: List[str] = []
        for n in nodes:
            hit = sub[(sub["nodes"] == n) & (sub["rpn"] == rpn) & (sub["benchmark"] == bench)]
            vals.append(float(hit[value_column].iloc[0]) if not hit.empty else 0.0)
            if annotate_sizes and not hit.empty:
                labels.append(format_bytes(int(hit["bytes"].iloc[0])))
            else:
                labels.append("")
        matrix[combo] = vals
        labels_by_combo[combo] = labels

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
        bars = ax.bar(
            [xi + offset for xi in x],
            matrix[combo],
            width=width,
            label=f"{rpn}rpn-{bench}",
        )
        if annotate_sizes:
            annotate_bars(ax, bars, labels_by_combo[combo])

    ax.set_title("Aggregate Bandwidth vs Nodes ({}, {})".format("GPU" if gpu_flag else "Host", title_suffix))
    ax.set_xlabel("Nodes")
    ax.set_ylabel(f"Bandwidth ({unit})")
    ax.set_xticks(x)
    ax.set_xticklabels([str(n) for n in nodes])
    ax.grid(axis="y", linestyle="--", alpha=0.3)
    ax.legend(ncol=3, fontsize=9)
    fig.tight_layout()
    figure_path = output_path.with_suffix(f".{fig_format}")
    debug_io(f"writing {figure_path}")
    fig.savefig(figure_path, dpi=dpi)
    plt.close(fig)


def plot_per_rpn(
    data: pd.DataFrame,
    gpu_flag: int,
    rpn: int,
    output_path: Path,
    fig_format: str,
    dpi: int,
    value_column: str,
    title_suffix: str,
    annotate_sizes: bool,
) -> None:
    sub = data[(data["gpu"] == gpu_flag) & (data["rpn"] == rpn)].copy()
    if sub.empty:
        debug_io(f"skipping plot {output_path.with_suffix(f'.{fig_format}')} because there is no data")
        return

    nodes = sorted_nodes(sub["nodes"])
    benches = ["read", "write", "writedata"]

    series: Dict[str, List[float]] = {}
    labels_by_bench: Dict[str, List[str]] = {}
    for bench in benches:
        vals: List[float] = []
        labels: List[str] = []
        for n in nodes:
            hit = sub[(sub["nodes"] == n) & (sub["benchmark"] == bench)]
            vals.append(float(hit[value_column].iloc[0]) if not hit.empty else 0.0)
            if annotate_sizes and not hit.empty:
                labels.append(format_bytes(int(hit["bytes"].iloc[0])))
            else:
                labels.append("")
        series[bench] = vals
        labels_by_bench[bench] = labels

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
        bars = ax.bar([xi + offset for xi in x], series[bench], width=width, label=bench)
        if annotate_sizes:
            annotate_bars(ax, bars, labels_by_bench[bench])

    ax.set_title(
        "Aggregate Bandwidth vs Nodes ({}, {} rank/node, {})".format(
            "GPU" if gpu_flag else "Host", rpn, title_suffix
        )
    )
    ax.set_xlabel("Nodes")
    ax.set_ylabel(f"Bandwidth ({unit})")
    ax.set_xticks(x)
    ax.set_xticklabels([str(n) for n in nodes])
    ax.grid(axis="y", linestyle="--", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    figure_path = output_path.with_suffix(f".{fig_format}")
    debug_io(f"writing {figure_path}")
    fig.savefig(figure_path, dpi=dpi)
    plt.close(fig)


def plot_message_size_curve(
    raw: pd.DataFrame,
    gpu_flag: int,
    rpn: int,
    benchmark: str,
    output_path: Path,
    fig_format: str,
    dpi: int,
) -> None:
    sub = raw[
        (raw["gpu"] == gpu_flag)
        & (raw["rpn"] == rpn)
        & (raw["benchmark"] == benchmark)
    ].copy()
    if sub.empty:
        debug_io(f"skipping plot {output_path.with_suffix(f'.{fig_format}')} because there is no data")
        return

    grouped = (
        sub.groupby(["nodes", "bytes"], as_index=False)["agg_bw_mb_s"]
        .max()
        .sort_values(["nodes", "bytes"])
    )
    if grouped.empty:
        debug_io(f"skipping plot {output_path.with_suffix(f'.{fig_format}')} because there is no data")
        return

    scaled_values, unit = scale_bandwidth(grouped["agg_bw_mb_s"].tolist())
    grouped = grouped.copy()
    grouped["plot_bw"] = scaled_values

    fig, ax = plt.subplots(figsize=(12, 7))
    for node in sorted_nodes(grouped["nodes"]):
        hit = grouped[grouped["nodes"] == node]
        ax.plot(hit["bytes"], hit["plot_bw"], marker="o", label=f"N={node}")

    ax.set_xscale("log", base=2)
    ax.set_title(
        "Bandwidth vs Message Size ({}, {} rank/node, {})".format(
            "GPU" if gpu_flag else "Host", rpn, benchmark
        )
    )
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel(f"Bandwidth ({unit})")
    ax.grid(axis="both", linestyle="--", alpha=0.3)
    ax.legend(title="Nodes")
    fig.tight_layout()
    figure_path = output_path.with_suffix(f".{fig_format}")
    debug_io(f"writing {figure_path}")
    fig.savefig(figure_path, dpi=dpi)
    plt.close(fig)


def main() -> None:
    args = parse_args()
    if args.mode == "size" and args.message_size is None:
        raise SystemExit("--message-size is required when --mode size is selected")

    input_dir = Path(args.input_dir).expanduser().resolve()
    output_dir = Path(args.output_dir).expanduser().resolve() if args.output_dir else input_dir / "plots"
    debug_io(f"resolved input_dir={input_dir}")
    debug_io(f"resolved output_dir={output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)

    raw = collect_rows(input_dir)

    raw_csv = output_dir / "raw_rows.csv"
    debug_io(f"writing {raw_csv}")
    raw.to_csv(raw_csv, index=False)
    summary = pd.DataFrame()

    if args.mode == "peak":
        summary = aggregate_peak(raw)
        summary_csv = output_dir / "peak_bandwidth.csv"
        debug_io(f"writing {summary_csv}")
        summary.to_csv(summary_csv, index=False)

        plot_overview(
            summary,
            gpu_flag=0,
            output_path=output_dir / "bandwidth_host_overview_peak",
            fig_format=args.format,
            dpi=args.dpi,
            value_column="peak_agg_bw_mb_s",
            title_suffix="peak BW",
            annotate_sizes=True,
        )
        plot_overview(
            summary,
            gpu_flag=1,
            output_path=output_dir / "bandwidth_gpu_overview_peak",
            fig_format=args.format,
            dpi=args.dpi,
            value_column="peak_agg_bw_mb_s",
            title_suffix="peak BW",
            annotate_sizes=True,
        )

        for gpu_flag in (0, 1):
            mode = "gpu" if gpu_flag else "host"
            for rpn in (1, 2, 4):
                plot_per_rpn(
                    summary,
                    gpu_flag=gpu_flag,
                    rpn=rpn,
                    output_path=output_dir / f"bandwidth_{mode}_rpn{rpn}_peak",
                    fig_format=args.format,
                    dpi=args.dpi,
                    value_column="peak_agg_bw_mb_s",
                    title_suffix="peak BW",
                    annotate_sizes=True,
                )
    elif args.mode == "size":
        assert args.message_size is not None
        summary = aggregate_for_message_size(raw, args.message_size)
        summary_csv = output_dir / f"bandwidth_{args.message_size}_bytes.csv"
        debug_io(f"writing {summary_csv}")
        summary.to_csv(summary_csv, index=False)

        suffix = f"{args.message_size}_bytes"
        title_suffix = f"msg size={format_bytes(args.message_size)}"
        plot_overview(
            summary,
            gpu_flag=0,
            output_path=output_dir / f"bandwidth_host_overview_{suffix}",
            fig_format=args.format,
            dpi=args.dpi,
            value_column="fixed_agg_bw_mb_s",
            title_suffix=title_suffix,
            annotate_sizes=False,
        )
        plot_overview(
            summary,
            gpu_flag=1,
            output_path=output_dir / f"bandwidth_gpu_overview_{suffix}",
            fig_format=args.format,
            dpi=args.dpi,
            value_column="fixed_agg_bw_mb_s",
            title_suffix=title_suffix,
            annotate_sizes=False,
        )

        for gpu_flag in (0, 1):
            mode = "gpu" if gpu_flag else "host"
            for rpn in (1, 2, 4):
                plot_per_rpn(
                    summary,
                    gpu_flag=gpu_flag,
                    rpn=rpn,
                    output_path=output_dir / f"bandwidth_{mode}_rpn{rpn}_{suffix}",
                    fig_format=args.format,
                    dpi=args.dpi,
                    value_column="fixed_agg_bw_mb_s",
                    title_suffix=title_suffix,
                    annotate_sizes=False,
                )
    else:
        for gpu_flag in (0, 1):
            mode = "gpu" if gpu_flag else "host"
            for rpn in (1, 2, 4):
                for benchmark in ("read", "write", "writedata"):
                    plot_message_size_curve(
                        raw,
                        gpu_flag=gpu_flag,
                        rpn=rpn,
                        benchmark=benchmark,
                        output_path=output_dir / f"curve_{mode}_{benchmark}_rpn{rpn}",
                        fig_format=args.format,
                        dpi=args.dpi,
                    )

    print(f"Parsed rows: {len(raw)}")
    print(f"Summary rows: {len(summary)}")
    print(f"Mode:        {args.mode}")
    if args.message_size is not None:
        print(f"Message size: {args.message_size} bytes ({format_bytes(args.message_size)})")
    print(f"Output dir:  {output_dir}")


if __name__ == "__main__":
    main()
