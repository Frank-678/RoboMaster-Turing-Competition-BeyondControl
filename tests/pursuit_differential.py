#!/usr/bin/env python3
"""Compare the ordinary-duel transition to a supplied existing arena runner.

This checks the current local arena, not the original ROS referee. The engine
must be built separately with the repository's normal scenario tooling.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pursuit_lab import DuelLab, action_json


def generate(seed, count):
    rng = random.Random(seed)
    for i in range(count):
        positions = rng.sample([(x, y) for y in range(5) for x in range(6)], 2)
        cells = [(x, y) for y in range(5) for x in range(6)
                 if (x, y) in positions or rng.random() > 0.22]
        resources = [{"position": list(p), "value": rng.choice([10, 20])}
                     for p in rng.sample(cells, min(4, len(cells)))]
        raw = {"schema": 1, "id": f"diff-{i}", "complete_local_model": True,
               "width": 6, "height": 5, "walkable": [list(p) for p in cells],
               "players": [{"position": list(p), "hp": rng.choice([10, 15, 20, 35, 100])}
                           for p in positions], "own_resource": rng.choice([0, 110, 120]),
               "own_damage": rng.choice([0, 59, 70, 290, 500, 800]),
               "resources": resources, "score_damage_cap": 300,
               "baseline_action": {"action": "defend"}}
        lab = DuelLab(raw)
        # Include unsuccessful occupied-cell moves and omission deliberately:
        # they have the same physical outcome even if the engine records a
        # rejection. Invalid-action score/tiebreak is outside this v1 model.
        actions = [("defend", -1), ("omit", -1)]
        actions += [(kind, d) for kind in ("attack", "move") for d in range(4)]
        yield raw, (rng.choice(actions), rng.choice(actions))


def as_engine_scene(raw, actions):
    walkable = {tuple(p) for p in raw["walkable"]}
    spawns = [p["position"] for p in raw["players"]]
    spawns += [list(p) for p in sorted(walkable) if list(p) not in spawns][:2]
    cars, agents = {}, {}
    for i, seat in enumerate("AB"):
        cars[seat] = dict(raw["players"][i], protected=False, protected_left=0,
                          R=raw["own_resource"] if i == 0 else 0,
                          D=raw["own_damage"] if i == 0 else 0)
        agents[seat] = {"type": "script", "event_delivery": "none", "actions": {},
                        "otherwise": action_json(actions[i])}
    return {"schema": 1, "name": raw["id"], "stage": 2, "tick_limit": 500,
            "qualified": True, "seed": 20261029,
            "map": {"width": raw["width"], "height": raw["height"], "vision_radius": 4,
                    "walls": [[x, y] for y in range(raw["height"]) for x in range(raw["width"])
                              if (x, y) not in walkable], "spawns": spawns},
            "initial": {"tick": 200, "resources": raw["resources"], "cars": cars},
            "missions": [], "events": [], "agents": agents, "run": {"until_tick": 201}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--count", type=int, default=240)
    parser.add_argument("--seed", type=int, default=20261029)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.count <= 10000:
        parser.error("count must be 1..10000")
    rows = []
    for raw, actions in generate(args.seed, args.count):
        lab = DuelLab(raw)
        expected = lab.step(lab.initial, actions)
        payload = {"scene": as_engine_scene(raw, actions), "build_id": "pursuit-differential"}
        row = {"id": raw["id"], "scene": raw, "actions": [action_json(a) for a in actions]}
        try:
            p = subprocess.run([str(args.engine.resolve())], input=json.dumps(payload),
                               text=True, capture_output=True, timeout=10)
            row.update(engine_rc=p.returncode, engine_stderr=p.stderr)
            if p.returncode:
                row.update(passed=False, error="engine failure", engine_stdout=p.stdout)
            else:
                run = json.loads(p.stdout)["runs"][0]
                final = run["world"]
                cars = final["cars"]
                actual = {"positions": [cars[s]["position"] for s in "AB"],
                          "hp": [cars[s]["hp"] for s in "AB"], "own_r": cars["A"]["R"],
                          "own_d": cars["A"]["D"], "own_deaths": cars["A"]["deaths"],
                          "resources": sorted(final["resources"], key=lambda r: r["position"])}
                wanted = {"positions": [list(p) for p in expected.positions], "hp": list(expected.hp),
                          "own_r": expected.own_r, "own_d": expected.own_d,
                          "own_deaths": expected.own_deaths,
                          "resources": [{"position": list(p), "value": v} for p, v in expected.resources]}
                row.update(passed=actual == wanted, expected=wanted, actual=actual)
        except (KeyError, ValueError, OSError, subprocess.TimeoutExpired) as exc:
            row.update(passed=False, error=f"{type(exc).__name__}: {exc}")
        rows.append(row)
    report = {"schema": 1, "scope": "ordinary transition vs supplied offline arena; not original ROS",
              "engine_sha256": hashlib.sha256(args.engine.read_bytes()).hexdigest(),
              "planner_sha256": hashlib.sha256((Path(__file__).resolve().parents[1] / "tools/pursuit_lab.py").read_bytes()).hexdigest(),
              "seed": args.seed, "planned": args.count, "completed": len(rows),
              "passed": sum(r["passed"] for r in rows), "rows": rows}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("planned", "completed", "passed", "engine_sha256")}))
    return 0 if report["passed"] == args.count else 1


if __name__ == "__main__":
    raise SystemExit(main())
