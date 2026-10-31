#!/usr/bin/env python3
"""新杠杆晋升评估：只在训练场景上做受控对照（冠军逐位一致 = 全部默认关）；
heldout 按协议只对最终选定组合评一次，不在本脚本内评。"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_arena  # 复用同一 build/scenes/evaluate/summarize，避免另起一套评估口径。


def groups(levers):
    base = {"name": "base_off", "parameters": {}}  # competition() 默认 = 三个杠杆全关的冠军基线。
    table = {
        "grace": [{"name": "grace1", "parameters": {"stale_grace_ticks": 1}},
                  {"name": "grace2", "parameters": {"stale_grace_ticks": 2}},
                  {"name": "grace3", "parameters": {"stale_grace_ticks": 3}}],
        "survival": [{"name": "worst_case", "parameters": {"worst_case_survival": True}}],
        "postcap": [{"name": "pc002", "parameters": {"post_cap_combat_weight": 0.02}},
                    {"name": "pc005", "parameters": {"post_cap_combat_weight": 0.05}},
                    {"name": "pc01", "parameters": {"post_cap_combat_weight": 0.1}}],
        "coverage": [{"name": "cov002", "parameters": {"explore_component_value": 0.002}},
                     {"name": "cov008", "parameters": {"explore_component_value": 0.008}},
                     {"name": "cov02", "parameters": {"explore_component_value": 0.02}}],
        "lowhp": [{"name": "lhp07", "parameters": {"low_hp_attack_discount": 0.7}},
                  {"name": "lhp04", "parameters": {"low_hp_attack_discount": 0.4}},
                  {"name": "lhp01", "parameters": {"low_hp_attack_discount": 0.1}}],
        "race": [{"name": "rrh10", "parameters": {"race_reach_holdoff": 1.0}},
                 {"name": "rrh08", "parameters": {"race_reach_holdoff": 0.8}},
                 {"name": "rrh05", "parameters": {"race_reach_holdoff": 0.5}}],
        "fortress": [{"name": "pf03", "parameters": {"prepare_fortress_value": 3.0}},
                     {"name": "pf06", "parameters": {"prepare_fortress_value": 6.0}},
                     {"name": "pf12", "parameters": {"prepare_fortress_value": 12.0}},
                     {"name": "pf24", "parameters": {"prepare_fortress_value": 24.0}}],
        "combined": [{"name": "combo_g1w_pc005",
                      "parameters": {"stale_grace_ticks": 1, "worst_case_survival": True,
                                     "post_cap_combat_weight": 0.05}},
                     {"name": "combo_g1w_pc01",
                      "parameters": {"stale_grace_ticks": 1, "worst_case_survival": True,
                                     "post_cap_combat_weight": 0.1}}],
    }
    return [base] + table[levers]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--levers", choices=sorted(set(["grace", "survival", "postcap", "combined", "coverage", "lowhp", "race", "fortress"])),
                        required=True)
    parser.add_argument("--report", required=True,
                        help="输出文件名（不含扩展名）：tests/results/levers-<report>.json")
    args = parser.parse_args()
    binary = run_arena.build(None)
    dumped = subprocess.run([str(binary), "--dump-competition"], capture_output=True, text=True, check=True)
    train = run_arena.scenes(False)  # 只用训练种子/席位；留出集留给最终组合的一次性验证。
    rows = run_arena.evaluate(binary, groups(args.levers), train)
    report = {
        "date": str(__import__("datetime").datetime.now(__import__("zoneinfo").ZoneInfo("Asia/Shanghai")).date()),
        "levers": args.levers, "scenes": train, "train_only": True,
        "competition_parameters": json.loads(dumped.stdout),
        "summaries": {name: run_arena.summarize(r) for name, r in rows.items()},
    }
    output = run_arena.ROOT / "tests/results"
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"levers-{args.report}.json"
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(destination), "summaries": report["summaries"]},
                     ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
