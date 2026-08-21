#!/usr/bin/env python3
"""Aggregate STRESS_OUTPUT=jsonl runs by benchmark configuration."""

import json
import math
import statistics
import sys
from collections import defaultdict


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "-"
    stream = sys.stdin if path == "-" else open(path, "r", encoding="utf-8")
    groups = defaultdict(list)
    try:
        for line in stream:
            line = line.strip()
            if not line or line.startswith("#") or not line.startswith("{"):
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            key = tuple(row.get(field) for field in (
                "workload", "pacing", "backend", "width", "height",
                "precision", "iterations", "layers", "blend_mode", "passes",
                "draw_calls", "sprites", "blur_taps", "program_switches", "batch", "texture_size", "texture_pattern", "texture_format", "texture_format_effective",
                "texture_filter", "texture_samples", "texture_layout",
            ))
            groups[key].append(row)
    finally:
        if stream is not sys.stdin:
            stream.close()

    for key, rows in sorted(groups.items(), key=lambda item: str(item[0])):
        fields = dict(zip((
            "workload", "pacing", "backend", "width", "height",
            "precision", "iterations", "layers", "blend_mode", "passes",
            "draw_calls", "sprites", "blur_taps", "program_switches", "batch", "texture_size", "texture_pattern", "texture_format", "texture_format_effective",
            "texture_filter", "texture_samples", "texture_layout",
        ), key))
        result = dict(fields)
        result["runs"] = len(rows)
        for source, target in (
            ("workload_fps", "workload_fps"),
            ("gpu_p50_ms", "gpu_p50_ms"),
            ("gpu_p95_ms", "gpu_p95_ms"),
            ("gpu_avg_ms", "gpu_avg_ms"),
            ("cpu_draw_avg_ms", "cpu_draw_avg_ms"),
            ("cpu_swap_avg_ms", "cpu_swap_avg_ms"),
            ("cpu_total_avg_ms", "cpu_total_avg_ms"),
            ("mpixel_s", "mpixel_s"),
            ("ns_pixel", "ns_pixel"),
        ):
            values = [float(row[source]) for row in rows
                      if isinstance(row.get(source), (int, float)) and math.isfinite(float(row[source]))]
            if not values:
                result[target] = None
                result[target + "_stddev"] = None
                result[target + "_min"] = None
                result[target + "_max"] = None
                continue
            result[target] = statistics.mean(values)
            result[target + "_stddev"] = statistics.stdev(values) if len(values) > 1 else 0.0
            result[target + "_min"] = min(values)
            result[target + "_max"] = max(values)
        print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
