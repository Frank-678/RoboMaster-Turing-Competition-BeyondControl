#!/usr/bin/env python3
"""Static validation for practice1 (20x20) and practice2 (24x24).

Does not access private referee ROS state or modify original practice files.
Optionally invokes the UNMODIFIED original practice/bin/validate_config executable.
"""
import argparse
import json
import subprocess
from collections import Counter, deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STANDARDS = {
    1: {"width": 20, "height": 20, "robots": 1},
    2: {"width": 24, "height": 24, "robots": 4},
}
EFFECTS = {
    "explosion": {"damage": 80, "duration_ticks": 1},
    "illusion": {"effect_ticks": 50},
    "angel": {"duration_ticks": 20, "effect_ticks": 10, "heal_per_tick": 10},
    "unrivaled": {"duration_ticks": 50, "stay_ticks": 10, "attack_damage": 50,
                  "defended_damage": 10, "attack_radius": 2},
    "world_split": {"duration_ticks": 30},
    "pepper": {"duration_ticks": 30, "damage": 50},
    "purity": {"duration_ticks": 20, "reflection_damage": 20},
    "pan": {"duration_ticks": 30, "movement_window_ticks": 20,
            "vulnerability_ticks": 30, "damage_multiplier": 2},
}
def verify(stage: int, cfg: dict) -> dict:
    require = lambda ok, why: ok or (_ for _ in ()).throw(ValueError(why))
    spec = STANDARDS[stage]
    require(cfg["stage"] == stage, "stage mismatch")
    w, h = cfg["map"]["width"], cfg["map"]["height"]
    require((w, h) == (spec["width"], spec["height"]), "dimensions mismatch")
    require(cfg.get("vision_radius") == 4, "vision radius is not 4")
    robots = cfg["robots"]
    require(len(robots) == spec["robots"], "unexpected robot count")
    seats = [r["team"] for r in robots]
    require(seats == (["A"] if stage == 1 else list("ABCD")), "unexpected seats")
    walls = {tuple(p) for p in cfg["map"]["obstacles"]}
    require(len(walls) == len(cfg["map"]["obstacles"]), "duplicate walls")
    coords = [tuple(r["spawn"]) for r in robots]
    require(len(coords) == len(set(coords)), "duplicate spawns")
    def coord(p):
        require(len(p) == 2 and all(type(v) is int for v in p), "invalid cell")
        require(0 <= p[0] < w and 0 <= p[1] < h, "out of map: " + str(p))
    for p in walls: coord(p)
    for p in coords:
        coord(p)
        require(p not in walls, "spawn on wall")
    rsrc = cfg["map"]["resources"]
    require(len(rsrc) == 16, "resource count must be 16")
    vals = Counter(r["value"] for r in rsrc)
    require(vals == Counter({10: 12, 20: 4}), "resource values not 12x10+4x20")
    points = [tuple(r["position"]) for r in rsrc]
    require(len(points) == len(set(points)), "duplicate resources")
    for p in points:
        coord(p)
        require(p not in walls, "resource on wall")
    # Avoid accepting unplayable maps with disconnected resource pockets.
    seen = {coords[0]}
    queue = deque([coords[0]])
    while queue:
        x, y = queue.popleft()
        for n in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if 0 <= n[0] < w and 0 <= n[1] < h and n not in walls and n not in seen:
                seen.add(n); queue.append(n)
    require(all(p in seen for p in coords + points), "spawn or resource disconnected")
    if stage == 1:
        require(not cfg["events"]["enabled"], "stage1 must have events disabled")
        require(not cfg["missions"], "stage1 must have no missions")
    else:
        require(len(cfg["missions"]) == 3, "expected three stage2 missions")
        require(cfg["events"]["enabled"], "stage2 events disabled")
        for mission in cfg["missions"]:
            if mission.get("type") == "reach":
                p = tuple(mission["position"]); coord(p)
                require(p not in walls, "reach target on wall")
                require(p in seen, "reach target unreachable")
                require(f"[{p[0]},{p[1]}]" in cfg["mission_text"],
                        "mission text disagrees with structured target")
        definitions = cfg["events"]["definitions"]
        for event, expected in EFFECTS.items():
            require(event in definitions, "missing event: " + event)
            for key, value in expected.items():
                require(definitions[event].get(key) == value,
                        f"{event}.{key} differs from v1.2 standard")
        for event in ("explosion", "illusion"):
            rect = definitions[event]["rect"]
            coord(rect["top_left"]); coord(rect["bottom_right"])
            require(all(a <= b for a,b in zip(rect["top_left"],rect["bottom_right"])),
                    "inverted event rectangle")
        for event in ("angel", "pepper"):
            for p in definitions[event]["points"]: coord(p)
        coord(definitions["world_split"]["point"])
    return {"stage":stage, "size":f"{w}x{h}", "robots":seats, "resources":len(rsrc),
            "total_value":sum(r["value"] for r in rsrc), "reachable_tiles":len(seen)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=int, choices=(1,2), required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--static-only", action="store_true",
                        help="skip original binary validator (for non-Ubuntu CI)")
    args = parser.parse_args()
    path = args.config or ROOT / f"practice{args.stage}/config.json"
    cfg = json.loads(path.read_text(encoding="utf-8"))
    print(json.dumps(verify(args.stage, cfg), ensure_ascii=False, sort_keys=True))
    if not args.static_only:
        exe = ROOT / "practice/bin/validate_config"
        proc = subprocess.run([str(exe)], input=json.dumps(cfg, ensure_ascii=False),
                              capture_output=True, text=True, timeout=10)
        print("original validator exit code:", proc.returncode)
        if proc.stdout: print(proc.stdout[:2000])
        if proc.stderr: print(proc.stderr[:3000])
        if proc.returncode != 0: raise SystemExit(proc.returncode or 1)
    print("PASS: static and requested original-validator checks completed")
if __name__ == "__main__":
    main()
