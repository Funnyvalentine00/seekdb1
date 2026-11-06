#!/usr/bin/env python3
"""Summarize preserved paired records without combining different datasets."""
import argparse
import json
from pathlib import Path
from statistics import median


def summarize(folder):
    rows = [json.loads(line) for line in (folder / "raw.jsonl").read_text().splitlines()]
    assert len({row["data_manifest_sha256"] for row in rows}) == 1
    sides = [[row for row in rows if row.get("variant", row["gemm"]) == v] for v in (0, 1)]
    assert len(sides[0]) == len(sides[1])
    pairs = sorted({row["pair"] for row in rows})
    metrics = {}
    for key in ("warm_build_ms", "single_qps", "batch16_qps", "batch100_qps"):
        def value(row):
            if key.startswith("batch"):
                return row["batch"][key[5:-4]]["qps"]
            return row[key]
        ratios = [value(next(row for row in sides[1] if row["pair"] == p)) /
                  value(next(row for row in sides[0] if row["pair"] == p)) for p in pairs]
        metrics[key] = dict(reference_median=median(map(value, sides[0])),
                            candidate_median=median(map(value, sides[1])),
                            paired_candidate_over_reference=ratios,
                            paired_percent_change=100 * (median(ratios) - 1))
    return dict(folder=folder.name, pairs=len(pairs), metrics=metrics,
                recalls=[sorted({(r["recall"]["10"], r["recall"]["100"]) for r in side}) for side in sides],
                max_sampled_vram_bytes=[max(r["sampled_peak_vram"] for r in side) for side in sides],
                max_post_free_growth_bytes=[max(r["vram_free_delta"] for r in side) for side in sides],
                bridge_sha256=[sorted({r["bridge_sha256"] for r in side}) for side in sides],
                manifest_sha256=rows[0]["data_manifest_sha256"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("folders", type=Path, nargs="+")
    args = parser.parse_args()
    print(json.dumps([summarize(folder) for folder in args.folders], indent=2))
