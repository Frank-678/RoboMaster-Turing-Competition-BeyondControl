#!/usr/bin/env python3
"""赛前自建对战、有限参数搜索和留出验证；不启动/修改practice，也不进行在线学习。"""
import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess
import sys
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = json.loads((ROOT / "tests/arena_settings.json").read_text())


def scenes(heldout):
    # 调参用两种种子并均衡席位；留出种子从未参与候选淘汰，且完整轮换A/B/C/D。
    result = []
    for i, style in enumerate(SETTINGS["map_styles"]):
        seeds = SETTINGS["heldout_seeds"] if heldout else SETTINGS["train_seeds"]
        for j, seed in enumerate(seeds):
            seed += i * 47
            result.append(dict(style=style, seed=seed, stage=1, seat=0,
                               opponents=["candidate", "collector", "collector", "collector"], delay=0, ai_success=False))
            seats = range(4) if heldout else [(i * 2 + j) % 4]
            for seat in seats:
                opponents = [""] * 4
                opponents[seat] = "candidate"
                pool = SETTINGS["opponent_styles"]
                for offset, kind in enumerate([pool[(i+j) % len(pool)], pool[(i+j+2) % len(pool)], "baseline"], 1):
                    opponents[(seat+offset) % 4] = kind
                result.append(dict(style=style, seed=seed, stage=2, seat=seat, opponents=opponents,
                                   delay=[0, 6, 10][(i+j+seat) % 3], ai_success=(i+j+seat) % 6 != 0))
    return result


def build(include):
    target = ROOT / "build/arena"
    target.mkdir(parents=True, exist_ok=True)
    flags = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(ROOT/"src/rmtc_bot/include")]
    if include:
        flags += ["-I", include]
    common = [str(ROOT/"src/rmtc_bot/src/events.cpp"), str(ROOT/"tests/arena.cpp"), str(ROOT/"tests/arena_scenarios.cpp"), str(ROOT/"tests/arena_opponents.cpp")]
    subprocess.run(flags + common + [str(ROOT/"tests/arena_test.cpp"), "-o", str(target/"rules_test")], check=True)
    subprocess.run([str(target/"rules_test")], check=True)
    subprocess.run(flags + common + [str(ROOT/"src/rmtc_bot/src/strategy.cpp"), str(ROOT/"src/rmtc_bot/src/tactics.cpp"),
                   str(ROOT/"tests/arena_main.cpp"), "-o", str(target/"arena")], check=True)
    return target/"arena"


def summarize(rows):
    grouped = defaultdict(list)
    for row in rows:
        grouped[row["scenario"]["stage"]].append(row)
    output = {"games": len(rows), "objective": 0, "invalid": 0, "deaths": 0, "blocked": 0,
              "decide_max_ms": max(row["decide_max_ms"] for row in rows)}
    objective = []
    for stage, games in grouped.items():
        own = [row["result"][row["focal"]] for row in games]
        score = statistics.mean(item["score"] for item in own)
        output[f"stage{stage}"] = {"games": len(games), "score_mean": score,
            "R_mean": statistics.mean(item["R"] for item in own), "D_mean": statistics.mean(item["D"] for item in own),
            "M_mean": statistics.mean(item["M"] for item in own), "deaths_mean": statistics.mean(item["deaths"] for item in own),
            "win_share": statistics.mean(row["win_share"] for row in games) if stage == 2 else None}
        for row, item in zip(games, own):
            # 这是调参目标，不是跨阶段官方总分；原始R/D/M和成绩仍另报。
            value = item["score"] / (40 if stage == 1 else 60)
            if stage == 2:
                value += 0.20 * row["win_share"] + 0.05 * row["score_margin"] / 60
            objective.append(value - 0.05 * item["invalid"])
            for key in ("invalid", "deaths", "blocked"):
                output[key] += item[key]
    output["objective"] = statistics.mean(objective)
    return output


def evaluate(binary, profiles, scenario_list):
    data = {"baseline_parameters": SETTINGS["baseline_parameters"], "profiles": profiles,
            "scenes": scenario_list, "trace_first": False}
    run = subprocess.run([str(binary)], input=json.dumps(data), text=True, capture_output=True, check=True)
    rows = [json.loads(line) for line in run.stdout.splitlines()]
    expected = len(profiles) * len(scenario_list)
    if len(rows) != expected:
        raise RuntimeError(f"Incomplete arena results: {len(rows)}/{expected}")
    return {profile["name"]: [row for row in rows if row["profile"] == profile["name"]] for profile in profiles}


def candidates(count):
    randomizer = random.Random(20261028)  # 固定搜索序列，不用赛场随机数或隐藏地图。
    base = SETTINGS["baseline_parameters"]
    result = [{"name": "baseline", "parameters": dict(base)},
              {"name": "new_default", "parameters": dict(base, tactical_search=True, resource_refinement=True, collision_replan=True, loop_recovery=True,
                  explore_tour_value=0.06)},
              {"name": "collision_only", "parameters": dict(base, collision_replan=True, loop_recovery=True)},
              {"name": "aggressive", "parameters": dict(base, tactical_search=True, resource_refinement=True, collision_replan=True, loop_recovery=True,
                  explore_tour_value=0.06,
                  tactical_risk=0.1, tactical_hp_value=0.005, tactical_death_cost=0.0, tactical_goal_weight=0.15,
                  resource_mission_bonus=2.5, reach_mission_value=6.0)}]
    ranges = {"resource_mission_bonus": [1.0, 2.5, 4.0, 5.0], "reach_mission_value": [4.0, 6.0, 8.0],
              "healing_below_hp": [40.0, 60.0, 80.0], "goal_retention_bonus": [0.0, 0.1, 0.25],
              "resource_plan_depth": [3, 4, 5], "tactical_risk": [0.1, 0.35, 0.6],
              "explore_toward_resource_value": [0.0, 0.25, 0.5, 1.0],
              "explore_tour_value": [0.0, 0.03, 0.06, 0.12],
              "mission_explore_value": [0.0, 0.25, 0.5, 1.0, 2.0],
              "frontier_value": [0.02, 0.03, 0.04, 0.05, 0.06, 0.08, 0.1, 0.2],
              "tactical_hp_value": [0.005, 0.015, 0.03], "tactical_death_cost": [0.0, 0.25, 1.0], "tactical_goal_weight": [0.15, 0.35, 0.65],
              "opponent_claim_discount": [1.0, 0.75, 0.5, 0.25],
              "stage1_flood_value": [0.0, 0.03, 0.05, 0.08, 0.12],
              "stage2_race_gate": [True, False],
              "stage2_explore_gate_tick": [0, 140, 160, 180, 200],
              "stage2_mission_pull_stale_ticks": [0, 15, 20, 30, 40]}
    for i in range(max(0, count-4)):
        values = dict(base, tactical_search=i % 5 != 4, resource_refinement=True, collision_replan=True, loop_recovery=True)
        values.update({key: randomizer.choice(choices) for key, choices in ranges.items()})
        result.append({"name": f"search_{i:02d}", "parameters": values})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tune", action="store_true", help="只在训练场景搜索，然后一次留出验证")
    parser.add_argument("--candidates", type=int, default=24)
    parser.add_argument("--json-include", help="仅测试构建时指定nlohmann/json.hpp目录，常规Ubuntu无需填写")
    parser.add_argument("--report", required=True,
                        help="本次报告名：写入arena-<report>.json与selected-parameters-<report>.json，绝不覆盖封存的arena-2026-10-28.json")
    args = parser.parse_args()
    binary = build(args.json_include)
    output = ROOT / "tests/results"
    output.mkdir(parents=True, exist_ok=True)
    report = {"date": str(datetime.now(ZoneInfo("Asia/Shanghai")).date()), "scope": SETTINGS["scope"],
              "settings": SETTINGS, "selection": "training only; heldout evaluated once after selection", "rounds": []}
    if args.tune:
        profiles = candidates(args.candidates)
        train = scenes(False)
        # 逐轮淘汰，增加的是训练地图；不会拿留出分数反复修改候选。
        schedule = [train[:6], train[:16], train]
        for round_number, subset in enumerate(schedule):
            rows = evaluate(binary, profiles, subset)
            ranked = sorted(profiles, key=lambda p: summarize(rows[p["name"]])["objective"], reverse=True)
            summaries = {p["name"]: summarize(rows[p["name"]]) for p in profiles}
            report["rounds"].append({"round": round_number, "scenes": subset, "profiles": profiles,
                                     "summaries": summaries})
            keep = 6 if round_number == 0 else 2
            if round_number < 2:
                profiles = ranked[:keep]
                # 基线必须参加每一轮；最后才可依据同一完整训练集判断改进。
                for reference in ("baseline", "collision_only"):
                    if not any(p["name"] == reference for p in profiles):
                        profiles.append(next(p for p in ranked if p["name"] == reference))
            else:
                profiles = ranked
            print(json.dumps({"round": round_number, "best": ranked[0]["name"], "summary": summaries[ranked[0]["name"]]}, ensure_ascii=False), flush=True)
        baseline_stage1 = summaries["baseline"]["stage1"]["score_mean"]
        eligible = [p for p in ranked if summaries[p["name"]]["invalid"] == 0 and
                    summaries[p["name"]]["stage1"]["score_mean"] >= baseline_stage1 - 1.0]
        selected = eligible[0]  # 排序只看训练集；采集平均退步超过1分或非法动作的候选不能入选。
        report["selection_guard"] = {"stage1_min_score": baseline_stage1 - 1.0, "invalid_max": 0,
                                     "eligible": [p["name"] for p in eligible], "selected_name": selected["name"]}
    else:
        # 非调参入口不再读取历史 selected-parameters.json（其中 4/4/0.1/0.25 与当前 match_config
        # 5/8/0/0 不一致，静默冒充当前配置）：评估对象就是编译进二进制的 competition()，
        # 生效参数由 arena --dump-competition 导出并写入报告，历史搜索产物保持历史身份。
        selected = {"name": "competition", "parameters": {}}
    # 独立留出不参与选择。额外消融使用同一选定参数，仅关闭一个新增模块。
    selected = {"name": "selected", "parameters": selected["parameters"]}
    comparison = [{"name": "baseline", "parameters": SETTINGS["baseline_parameters"]}, selected,
                  {"name": "selected_without_tactics", "parameters": dict(selected["parameters"], tactical_search=False)},
                  {"name": "selected_without_refinement", "parameters": dict(selected["parameters"], resource_refinement=False)},
                  {"name": "selected_without_collision_replan", "parameters": dict(selected["parameters"], collision_replan=False)},
                  {"name": "selected_without_toward", "parameters": dict(selected["parameters"], explore_toward_resource_value=0.0)},
                  {"name": "selected_without_tour", "parameters": dict(selected["parameters"], explore_tour_value=0.0)}]
    heldout = scenes(True)
    results = evaluate(binary, comparison, heldout)
    report.update(selected=selected, heldout_scenes=heldout, heldout_results=results,
                  heldout_summaries={name: summarize(rows) for name, rows in results.items()})
    hashes = {}
    for path in ["src/rmtc_bot/src/strategy.cpp", "src/rmtc_bot/src/tactics.cpp", "src/rmtc_bot/include/rmtc_bot/match_config.hpp", "tests/arena.cpp", "tests/arena_scenarios.cpp", "tests/arena_opponents.cpp"]:
        hashes[path] = hashlib.sha256((ROOT/path).read_bytes()).hexdigest()
    report["source_sha256"] = hashes
    # 最终生效参数与二进制指纹：每份报告自证“测的就是当前比赛程序”，不依赖参数名推断。
    dumped = subprocess.run([str(binary), "--dump-competition"], capture_output=True, text=True, check=True)
    report["competition_parameters"] = json.loads(dumped.stdout)
    report["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    destination = output / f"arena-{args.report}.json"
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    (output / f"selected-parameters-{args.report}.json").write_text(json.dumps(selected, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(destination), "heldout": report["heldout_summaries"]}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
