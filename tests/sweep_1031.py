#!/usr/bin/env python3
"""10-31 冲刺海扫：全未测旋钮族一次性离线评估（train 24 场景）。
口径同 eval_levers：base 对照 + 逐家族参数注入；heldout 只留给最终组合。
用法: python3 tests/sweep_1031.py --report 1031  (输出 tests/results/sweep-1031.json)
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_arena

BASE = {"name": "base", "parameters": {}}
TABLE = {
    "frt": [{"name": "frt04", "parameters": {"stage1_frontier_value": 0.04}},
            {"name": "frt05", "parameters": {"stage1_frontier_value": 0.05}},
            {"name": "frt06", "parameters": {"stage1_frontier_value": 0.06}},
            {"name": "frt07", "parameters": {"stage1_frontier_value": 0.07}},
            {"name": "frt08", "parameters": {"stage1_frontier_value": 0.08}}],
    "heal": [{"name": "heal20", "parameters": {"healing_below_hp": 20.0}},
             {"name": "heal60", "parameters": {"healing_below_hp": 60.0}},
             {"name": "heal80", "parameters": {"healing_below_hp": 80.0}}],
    "haz": [{"name": "haz0", "parameters": {"hazard_lead_ticks": 0}},
            {"name": "haz4", "parameters": {"hazard_lead_ticks": 4}},
            {"name": "haz6", "parameters": {"hazard_lead_ticks": 6}}],
    "occ": [{"name": "occ05", "parameters": {"opponent_contest_cost": 0.5}},
            {"name": "occ20", "parameters": {"opponent_contest_cost": 2.0}}],
    "pcs": [{"name": "pcs01", "parameters": {"opponent_pressure_cost": 0.1}},
            {"name": "pcs05", "parameters": {"opponent_pressure_cost": 0.5}}],
    "tac": [{"name": "tac_on", "parameters": {"tactical_search": True}},
            {"name": "tac_r03", "parameters": {"tactical_search": True, "tactical_risk": 0.3}},
            {"name": "tac_r08", "parameters": {"tactical_search": True, "tactical_risk": 0.8}}],
    "loop": [{"name": "lw08", "parameters": {"loop_window_ticks": 8}},
             {"name": "lw16", "parameters": {"loop_window_ticks": 16}},
             {"name": "rc06", "parameters": {"recovery_ticks": 6}},
             {"name": "rc20", "parameters": {"recovery_ticks": 20}}],
    "bools": [{"name": "nocolr", "parameters": {"collision_replan": False}},
              {"name": "noref", "parameters": {"resource_refinement": False}}],
    "stall": [{"name": "stl12", "parameters": {"stalled_goal_cooldown_ticks": 12}},
              {"name": "stl48", "parameters": {"stalled_goal_cooldown_ticks": 48}}],
    "trials": [{"name": "tri6", "parameters": {"resource_refinement_trials": 6}},
               {"name": "tri24", "parameters": {"resource_refinement_trials": 24}}],
    "grace": [{"name": "grc0", "parameters": {"stale_grace_ticks": 0}},
              {"name": "grc20", "parameters": {"stale_grace_ticks": 20}}],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    binary = run_arena.build(None)
    train = run_arena.scenes(False)
    configs = [BASE] + [c for k in TABLE for c in TABLE[k]]
    rows = run_arena.evaluate(binary, configs, train)
    report = {
        "date": str(__import__("datetime").datetime.now(__import__("zoneinfo").ZoneInfo("Asia/Shanghai")).date()),
        "sweep": "1031", "scenes": train, "train_only": True,
        "summaries": {name: run_arena.summarize(r) for name, r in rows.items()},
    }
    outdir = run_arena.ROOT / "tests/results"
    outdir.mkdir(parents=True, exist_ok=True)
    dest = outdir / f"sweep-{args.report}.json"
    dest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(dest), "summaries": report["summaries"]}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
