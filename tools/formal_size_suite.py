#!/usr/bin/env python3
"""Generate reproducible, size-aligned SYNTHETIC RMTC map fixtures.

Uses only official map sizes and original published practice config shape.
The generated maps/events are never official tournament truth.
"""
import argparse
import copy
import hashlib
import json
import random
from pathlib import Path
from formal_practice_check import verify

ROOT = Path(__file__).resolve().parents[1]
STYLES = ("uniform", "cluster", "edge", "center", "spread")

def generate(stage, seed, style, n_walls):
    rng = random.Random(seed)
    cfg = json.loads((ROOT / f"practice{stage}/config.json").read_text(encoding="utf-8"))
    width, height = cfg["map"]["width"], cfg["map"]["height"]
    spawns = {tuple(car["spawn"]) for car in cfg["robots"]}
    reserved = set(spawns)
    if stage == 2:
        for task in cfg["missions"]:
            if task.get("type") == "reach":
                reserved.add(tuple(task["position"]))
        d = cfg["events"]["definitions"]
        for kind in ("angel", "pepper"):
            reserved.update(map(tuple, d[kind]["points"]))
        reserved.add(tuple(d["world_split"]["point"]))
    cells = [(x, y) for x in range(width) for y in range(height)]
    wall_candidates = [c for c in cells if c not in reserved]
    if n_walls > len(wall_candidates) - 16:
        raise ValueError("too many walls")
    for attempt in range(300):
        trial = copy.deepcopy(cfg)
        obstacles = set(rng.sample(wall_candidates, n_walls))
        trial["map"]["obstacles"] = [list(p) for p in sorted(obstacles)]
        available = [p for p in cells if p not in obstacles and p not in reserved]
        rng.shuffle(available)
        if style == "cluster":
            centers = rng.sample(available, 2)
            available.sort(key=lambda p: min(abs(p[0]-c[0])+abs(p[1]-c[1]) for c in centers))
        elif style == "edge":
            available.sort(key=lambda p: min(p[0], p[1], width-1-p[0], height-1-p[1]))
        elif style == "center":
            available.sort(key=lambda p: abs(p[0]-width//2)+abs(p[1]-height//2))
        elif style == "spread":
            chosen = []
            remaining = available[:]
            while remaining and len(chosen) < 16:
                if not chosen:
                    nxt = remaining.pop(0)
                else:
                    nxt = max(remaining, key=lambda p: min(abs(p[0]-q[0])+abs(p[1]-q[1]) for q in chosen))
                    remaining.remove(nxt)
                chosen.append(nxt)
            available = chosen + remaining
        chosen = available[:16]
        vals = [20]*4 + [10]*12
        rng.shuffle(vals)
        trial["map"]["resources"] = [
            {"id":f"synthetic_{i:02d}", "position":list(pt), "value":vals[i]}
            for i,pt in enumerate(chosen)
        ]
        try:
            stats = verify(stage, trial)
        except (ValueError, KeyError):
            continue
        return trial, dict(stats, seed=seed, style=style, wall_count=n_walls, attempts=attempt+1)
    raise RuntimeError("could not produce a valid connected scenario in 300 attempts")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", type=int, choices=(1, 2), required=True)
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--seed", type=int, default=20261030)
    ap.add_argument("--count", type=int, default=2)
    ap.add_argument("--walls", type=int, default=18)
    ap.add_argument("--style", choices=STYLES + ("all",), default="all")
    a = ap.parse_args()
    if a.count < 1 or a.count > 100: ap.error("count must be 1..100")
    if not 0 <= a.walls <= 90: ap.error("walls must be 0..90")
    a.output_dir.mkdir(parents=True, exist_ok=True)
    styles = STYLES if a.style == "all" else (a.style,)
    manifest = []
    for index in range(a.count):
        for offset, style in enumerate(styles):
            seed = a.seed + 101*index + offset
            cfg, stats = generate(a.stage, seed, style, a.walls)
            path = a.output_dir / f"stage{a.stage}-{style}-{seed}.json"
            data = json.dumps(cfg, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
            path.write_text(data, encoding="utf-8")
            manifest.append(dict(stats, file=path.name, sha256=hashlib.sha256(data.encode()).hexdigest()))
    (a.output_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(manifest)} synthetic stage-{a.stage} maps: {a.output_dir}")

if __name__ == "__main__":
    main()
