#!/usr/bin/env python3
"""10-31 冲刺海扫第二批：arena 参数表内剩余未测旋钮（--group b1|b2 拆进程并发）。
口径同 sweep_1031；各组带 base 锚点；heldout 只留给最终组合。
用法: python3 tests/sweep_1031b.py --report 1031b --group b1
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_arena

BASE = {"name": "base", "parameters": {}}
TABLES = {
    "b1": [{"name": "thp03", "parameters": {"tactical_hp_value": 0.03}},
           {"name": "thp06", "parameters": {"tactical_hp_value": 0.06}},
           {"name": "tfd05", "parameters": {"tactical_future_discount": 0.5}},
           {"name": "tfd08", "parameters": {"tactical_future_discount": 0.8}},
           {"name": "tgw065", "parameters": {"tactical_goal_weight": 0.65}},
           {"name": "tgw085", "parameters": {"tactical_goal_weight": 0.85}},
           {"name": "tdc05", "parameters": {"tactical_death_cost": 0.5}},
           {"name": "tdc20", "parameters": {"tactical_death_cost": 2.0}},
           {"name": "wcs_on", "parameters": {"worst_case_survival": True}},
           {"name": "rrh03", "parameters": {"race_reach_holdoff": 0.3}},
           {"name": "rrh06", "parameters": {"race_reach_holdoff": 0.6}}],
    "b2": [{"name": "srd0005", "parameters": {"stale_resource_decay": 0.005}},
           {"name": "srd002", "parameters": {"stale_resource_decay": 0.02}},
           {"name": "rmb3", "parameters": {"resource_mission_bonus": 3.0}},
           {"name": "rmb7", "parameters": {"resource_mission_bonus": 7.0}},
           {"name": "rmv5", "parameters": {"reach_mission_value": 5.0}},
           {"name": "rmv12", "parameters": {"reach_mission_value": 12.0}},
           {"name": "sfv002", "parameters": {"stage1_flood_value": 0.02}},
           {"name": "sfv005", "parameters": {"stage1_flood_value": 0.05}},
           {"name": "s2pull3", "parameters": {"stage2_mission_pull_stale_ticks": 3}},
           {"name": "s2pull6", "parameters": {"stage2_mission_pull_stale_ticks": 6}},
           {"name": "rpd2", "parameters": {"resource_plan_depth": 2}},
           {"name": "rpd4", "parameters": {"resource_plan_depth": 4}},
           {"name": "rbw3", "parameters": {"resource_beam_width": 3}},
           {"name": "rbw7", "parameters": {"resource_beam_width": 7}},
           {"name": "rcl6", "parameters": {"resource_candidate_limit": 6}},
           {"name": "rcl12", "parameters": {"resource_candidate_limit": 12}},
           {"name": "rpb5", "parameters": {"resource_plan_budget_ms": 5}},
           {"name": "rpb20", "parameters": {"resource_plan_budget_ms": 20}}],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    parser.add_argument("--group", required=True, choices=list(TABLES))
    args = parser.parse_args()
    binary = run_arena.build(None)
    train = run_arena.scenes(False)
    configs = [BASE] + TABLES[args.group]
    rows = run_arena.evaluate(binary, configs, train)
    report = {
        "date": str(__import__("datetime").datetime.now(__import__("zoneinfo").ZoneInfo("Asia/Shanghai")).date()),
        "sweep": "1031b", "group": args.group, "scenes": train, "train_only": True,
        "summaries": {name: run_arena.summarize(r) for name, r in rows.items()},
    }
    dest = run_arena.ROOT / "tests/results" / f"sweep-{args.report}-{args.group}.json"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(dest), "group": args.group,
                      "summaries": report["summaries"]}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
