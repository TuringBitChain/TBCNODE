#!/usr/bin/env python3
# Copyright (c) 2026 The Open TBC developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""
Plot the CTxMemPool::GetMinFee admission curve as an SVG.

The implementation mirrors src/txmempool.cpp:

  if usage <= N1 or N2 <= N1:
      rate = floor
  elif usage >= N2:
      rate = cap
  else:
      rate = floor * (N2 - N1) / (N2 - usage)

All fee rates are in satoshis per kB, matching CFeeRate::GetFeePerK().
"""

import argparse
import math
from pathlib import Path


ONE_MEGABYTE = 1_000_000
DEFAULT_FLOOR_RATE = 60
DEFAULT_RAMP_START_MB = 500
DEFAULT_MAX_MEMPOOL_MB = 1000
DEFAULT_CAP_RATE = 1 << 40
DEFAULT_POINTS = 400


def get_min_fee(usage_bytes: int, floor_rate: int, ramp_start_bytes: int,
                max_mempool_bytes: int, cap_rate: int) -> int:
    n1 = ramp_start_bytes
    n2 = max_mempool_bytes

    if usage_bytes <= n1 or n2 <= n1:
        return floor_rate
    if usage_bytes >= n2:
        return cap_rate

    rate = (floor_rate * (n2 - n1)) // (n2 - usage_bytes)
    return min(rate, cap_rate)


def sample_curve(floor_rate: int, ramp_start_mb: float, max_mempool_mb: float,
                 cap_rate: int, points: int):
    n1 = int(ramp_start_mb * ONE_MEGABYTE)
    n2 = int(max_mempool_mb * ONE_MEGABYTE)
    points = max(points, 2)

    samples = []
    last_usage = max(int(n2 * 0.98), 0)
    for index in range(points):
        usage = int(last_usage * index / (points - 1))
        rate = get_min_fee(usage, floor_rate, n1, n2, cap_rate)
        samples.append((usage / ONE_MEGABYTE, rate))
    return samples, n1 / ONE_MEGABYTE, n2 / ONE_MEGABYTE


def format_rate(rate: float) -> str:
    if rate >= 1_000_000:
        return f"{rate / 1_000_000:.1f}M"
    if rate >= 1_000:
        return f"{rate / 1_000:.1f}k"
    return f"{rate:.0f}"


def svg_escape(text: str) -> str:
    return (text.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;"))


def render_svg(samples, ramp_start_mb: float, max_mempool_mb: float,
               floor_rate: int, cap_rate: int, y_scale: str, output: Path):
    width = 1100
    height = 720
    margin_left = 100
    margin_right = 40
    margin_top = 70
    margin_bottom = 90
    plot_width = width - margin_left - margin_right
    plot_height = height - margin_top - margin_bottom

    max_rate = max(rate for _, rate in samples)
    y_top = max_rate
    if y_top <= 0:
        y_top = 1

    def x_px(usage_mb: float) -> float:
        return margin_left + (usage_mb / max_mempool_mb) * plot_width

    if y_scale == "log":
        y_floor = max(float(floor_rate), 1.0)
        y_ceiling = max(float(y_top), y_floor * 10.0)
        log_floor = math.log10(y_floor)
        log_ceiling = math.log10(y_ceiling)

        def y_px(rate: float) -> float:
            log_rate = math.log10(max(float(rate), y_floor))
            if log_ceiling == log_floor:
                return margin_top + plot_height / 2
            return margin_top + plot_height * (1.0 - ((log_rate - log_floor) / (log_ceiling - log_floor)))

        tick_values = [
            10 ** (log_floor + (log_ceiling - log_floor) * idx / 6)
            for idx in range(7)
        ]
    else:
        def y_px(rate: float) -> float:
            return margin_top + plot_height * (1.0 - (rate / y_top))

        tick_values = [
            y_top * idx / 6
            for idx in range(7)
        ]

    polyline = " ".join(f"{x_px(usage_mb):.2f},{y_px(rate):.2f}" for usage_mb, rate in samples)

    x_ticks = 8
    lines = []
    labels = []

    for rate in tick_values:
        py = y_px(rate)
        lines.append(
            f'<line x1="{margin_left}" y1="{py:.2f}" x2="{width - margin_right}" y2="{py:.2f}" '
            'stroke="#d7ded9" stroke-width="1"/>')
        labels.append(
            f'<text x="{margin_left - 14}" y="{py + 5:.2f}" text-anchor="end" '
            'font-size="14" fill="#29443a">{}</text>'.format(svg_escape(format_rate(rate))))

    for idx in range(x_ticks + 1):
        usage_mb = max_mempool_mb * idx / x_ticks
        px = x_px(usage_mb)
        lines.append(
            f'<line x1="{px:.2f}" y1="{margin_top}" x2="{px:.2f}" y2="{height - margin_bottom}" '
            'stroke="#edf2ee" stroke-width="1"/>')
        labels.append(
            f'<text x="{px:.2f}" y="{height - margin_bottom + 28}" text-anchor="middle" '
            f'font-size="14" fill="#29443a">{usage_mb:.0f}</text>')

    ramp_x = x_px(ramp_start_mb)
    cap_x = x_px(max_mempool_mb)
    ramp_y = y_px(floor_rate)

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
  <rect width="100%" height="100%" fill="#f7faf8"/>
  <text x="{margin_left}" y="38" font-size="28" font-family="monospace" fill="#163126">CTxMemPool::GetMinFee Curve</text>
  <text x="{margin_left}" y="60" font-size="15" font-family="monospace" fill="#48665a">
    floor={floor_rate} sat/kB, N1={ramp_start_mb:.0f} MB, N2={max_mempool_mb:.0f} MB, cap={cap_rate} sat/kB, y={y_scale}
  </text>
  <rect x="{margin_left}" y="{margin_top}" width="{plot_width}" height="{plot_height}" fill="#ffffff" stroke="#b9c9c0" stroke-width="1.5"/>
  {''.join(lines)}
  {''.join(labels)}
  <line x1="{margin_left}" y1="{height - margin_bottom}" x2="{width - margin_right}" y2="{height - margin_bottom}" stroke="#163126" stroke-width="2"/>
  <line x1="{margin_left}" y1="{margin_top}" x2="{margin_left}" y2="{height - margin_bottom}" stroke="#163126" stroke-width="2"/>
  <line x1="{ramp_x:.2f}" y1="{margin_top}" x2="{ramp_x:.2f}" y2="{height - margin_bottom}" stroke="#b45f06" stroke-dasharray="8 6" stroke-width="2"/>
  <line x1="{cap_x:.2f}" y1="{margin_top}" x2="{cap_x:.2f}" y2="{height - margin_bottom}" stroke="#a61b29" stroke-dasharray="8 6" stroke-width="2"/>
  <polyline fill="none" stroke="#0d7a5f" stroke-width="4" points="{polyline}"/>
  <circle cx="{ramp_x:.2f}" cy="{ramp_y:.2f}" r="5" fill="#b45f06"/>
  <text x="{ramp_x + 10:.2f}" y="{margin_top + 24}" font-size="14" font-family="monospace" fill="#8b4a08">N1 ramp start</text>
  <text x="{cap_x - 10:.2f}" y="{margin_top + 24}" text-anchor="end" font-size="14" font-family="monospace" fill="#7d1020">N2 hard cap</text>
  <text x="{margin_left + plot_width / 2:.2f}" y="{height - 24}" text-anchor="middle" font-size="18" font-family="monospace" fill="#163126">Mempool usage (MB)</text>
  <text x="28" y="{margin_top + plot_height / 2:.2f}" text-anchor="middle" font-size="18" font-family="monospace" fill="#163126" transform="rotate(-90 28,{margin_top + plot_height / 2:.2f})">Required fee rate (sat/kB)</text>
</svg>
"""
    output.write_text(svg, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Plot CTxMemPool::GetMinFee as an SVG.")
    parser.add_argument("--floor-rate", type=int, default=DEFAULT_FLOOR_RATE,
                        help="Flat admission floor in sat/kB below the ramp start.")
    parser.add_argument("--ramp-start-mb", type=float, default=DEFAULT_RAMP_START_MB,
                        help="Mempool usage N1 in MB below which the floor applies.")
    parser.add_argument("--max-mempool-mb", type=float, default=DEFAULT_MAX_MEMPOOL_MB,
                        help="Hard mempool cap N2 in MB.")
    parser.add_argument("--cap-rate", type=int, default=DEFAULT_CAP_RATE,
                        help="Upper fee-rate clamp in sat/kB.")
    parser.add_argument("--y-scale", choices=("linear", "log"), default="linear",
                        help="Y-axis scale for the rendered chart.")
    parser.add_argument("--points", type=int, default=DEFAULT_POINTS,
                        help="Number of sampled points across the curve.")
    parser.add_argument("--output", type=Path,
                        default=Path("mempool_min_fee_curve.svg"),
                        help="Output SVG path.")
    args = parser.parse_args()

    samples, ramp_start_mb, max_mempool_mb = sample_curve(
        args.floor_rate,
        args.ramp_start_mb,
        args.max_mempool_mb,
        args.cap_rate,
        args.points,
    )
    if args.y_scale == "log" and args.floor_rate <= 0:
        raise SystemExit("--y-scale log requires --floor-rate > 0")

    render_svg(samples, ramp_start_mb, max_mempool_mb,
               args.floor_rate, args.cap_rate, args.y_scale, args.output)

    print(f"Wrote {args.output}")
    print("Formula:")
    print("  usage <= N1 or N2 <= N1  => floor")
    print("  usage >= N2              => cap")
    print("  else                     => floor * (N2 - N1) / (N2 - usage)")


if __name__ == "__main__":
    main()
