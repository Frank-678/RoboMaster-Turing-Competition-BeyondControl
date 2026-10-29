#!/usr/bin/env python3
"""Emit reproducible duel micro-scenes and full root matrices, no API calls."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import sys

from pursuit_lab import DuelLab


def fixture(name, positions, hp=(100, 100), walls=(), corridor=False,
            resources=(), goal=None, baseline=("defend", None), own_d=0):
    board = [(x, y) for y in range(5) for x in range(7)
             if ((y == 2) if corridor else True) and (x, y) not in walls]
    act = {"action": baseline[0]}
    if baseline[1] is not None:
        act["direction"] = baseline[1]
    return {"schema": 1, "id": name, "complete_local_model": True,
            "width": 7, "height": 5, "walkable": [list(p) for p in board],
            "players": [{"position": list(p), "hp": h} for p, h in zip(positions, hp)],
            "own_damage": own_d, "resources": [{"position": list(p), "value": v} for p, v in resources],
            "baseline_action": act, "goal": list(goal) if goal is not None else None,
            "search": {"horizon": 2, "joint_budget": 30000, "time_budget_ms": 200,
                       "matrix_iterations": 1024}}


def fixtures():
    return [
        fixture("corner-immediate-shot", ((6, 1), (6, 0)), hp=(100, 10), baseline=("move", "left")),
        fixture("corner-healthy-mobile", ((6, 1), (6, 0)), baseline=("move", "left")),
        fixture("diagonal-edge", ((5, 1), (6, 0)), hp=(100, 20), baseline=("move", "right")),
        fixture("wall-crossfire", ((1, 2), (3, 2)), walls=((2, 2),), baseline=("move", "up")),
        fixture("bridge-contested", ((1, 2), (3, 2)), corridor=True, baseline=("move", "right")),
        fixture("lethal-current-ray", ((2, 2), (3, 2)), hp=(20, 100), own_d=100,
                baseline=("move", "up")),
        fixture("early-side-exit", ((1, 2), (4, 2)), hp=(20, 100), own_d=100,
                goal=(0, 1), baseline=("move", "right")),
        fixture("economy-versus-chase", ((1, 2), (4, 2)), own_d=800,
                resources=(((1, 1), 20), ((0, 1), 10)), goal=(1, 1), baseline=("move", "right")),
        fixture("obstacle-detour", ((1, 2), (4, 2)), walls=((2, 1), (2, 2), (2, 3)),
                resources=(((1, 0), 20),), goal=(1, 0), baseline=("move", "down")),
        fixture("symmetric-open-no-shot", ((1, 1), (4, 3)), baseline=("move", "right")),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for base in fixtures():
        for cap in (300, 500):
            scene = dict(base, score_damage_cap=cap, id=f"{base['id']}-cap{cap}")
            raw = json.dumps(scene, ensure_ascii=False, indent=2) + "\n"
            (args.out / f"{scene['id']}.scene.json").write_text(raw, encoding="utf-8")
            result = DuelLab(scene).search(sample_seed=20261029)
            result.update(scene_id=scene["id"], scene_sha256=hashlib.sha256(raw.encode()).hexdigest())
            (args.out / f"{scene['id']}.result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            rows.append(result)
    summary = {"schema": 1, "scope": "20 designed local games; no match win-rate or qualification estimate",
               "planner_sha256": hashlib.sha256((Path(__file__).parent / "pursuit_lab.py").read_bytes()).hexdigest(),
               "planned": 20, "completed": len(rows),
               "completed_depth_histogram": {str(d): sum(r["completed_depth"] == d for r in rows) for d in range(3)},
               "max_elapsed_ms": max(r["elapsed_ms"] for r in rows),
               "median_elapsed_ms": statistics.median(r["elapsed_ms"] for r in rows), "rows": rows}
    (args.out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("planned", "completed", "completed_depth_histogram", "max_elapsed_ms", "median_elapsed_ms")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
