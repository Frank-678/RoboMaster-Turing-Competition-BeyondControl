#!/usr/bin/env python3
"""导航脱困回归：固定六个对抗场景，可选同夹具比较旧源码/二进制，不启动ROS或LLM。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(source_root, target, json_include):
    # 只有生产核心来自指定版本；两边都使用当前同一份测试裁判和场景。
    core = source_root / "src/rmtc_bot"
    flags = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(core / "include")]
    if json_include:
        flags += ["-I", json_include]
    sources = [core / "src" / (name + ".cpp") for name in ("strategy", "tactics", "events")]
    sources += [ROOT / "tests/arena.cpp", ROOT / "tests/unstuck_benchmark.cpp"]
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(flags + [str(p) for p in sources] + ["-o", str(target)], check=True)
    return target


def run(binary, version):
    completed = subprocess.run([str(binary.resolve()), version], text=True, capture_output=True, check=True, timeout=60)
    rows = [json.loads(line) for line in completed.stdout.splitlines()]
    if len(rows) != 6:
        raise AssertionError("expected six complete 500-tick cases")
    return {r["name"]: r for r in rows}


def verify(rows):
    # 检查长期无益循环、争格脱离、真实无出口，以及正常绕路/回头采集均保留。
    for row in rows.values():
        assert row["ticks"] == 500 and row["result"]["invalid"] == 0, row["name"]
    for name in ("top_edge_reflection", "top_edge_reflection_short"):
        assert rows[name]["result"]["R"] >= 20, name
        assert rows[name]["longest_periodic_motion_without_score_progress"] <= 24, name
    symmetric = rows["symmetric_contention"]
    assert 0 <= symmetric["first_task_tick"] < 80 and symmetric["result"]["blocked"] < 100
    closed = rows["closed_two_cells"]
    assert closed["longest_periodic_motion_without_score_progress"] <= 24
    assert closed["reasons"].get("stuck_no_safe_exit", 0) >= 450
    detour = rows["wall_detour"]
    assert detour["result"]["R"] == 20 and 0 <= detour["first_task_tick"] < 50
    assert rows["long_corridor"]["result"]["R"] == 30


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-include", help="离线构建的JSON头文件目录，系统已安装时不填")
    parser.add_argument("--baseline-root", type=Path, help="旧版工程根目录；只读其中生产核心")
    parser.add_argument("--before-binary", type=Path, help="已编译的旧版同夹具二进制，可省去重编译")
    parser.add_argument("--after-binary", type=Path, help="已编译的新版同夹具二进制")
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument("--output", type=Path, default=ROOT / "build/unstuck/latest.json")
    args = parser.parse_args()
    target = ROOT / "build/unstuck"
    after_binary = args.after_binary or build(ROOT, target / "after", args.json_include)
    after = run(after_binary, "loop-recovery")
    verify(after)
    before_binary = args.before_binary
    if args.baseline_root:
        before_binary = build(args.baseline_root, target / "before", args.json_include)
    before = run(before_binary, args.baseline_label) if before_binary else None
    report = {"date": "2026-10-28", "scope": "Six synthetic navigation cases, not user map, original referee or real AI",
              "baseline": args.baseline_label if before else None, "before": before, "after": after,
              "checks": "six 500-tick navigation cases passed; compile warnings are errors",
              "limitations": ["500-tick reflection is a stress case, not an assertion of official event duration",
                              "Unused cars held inactive by a test-only respawn counter; production respawn rules unchanged",
                              "Lethal combat and announced hazard guards retain priority",
                              "Original referee/DDS and the reported teammate map not tested"]}
    paths = list((ROOT / "src/rmtc_bot/include/rmtc_bot").glob("*.hpp"))
    paths += [ROOT / "src/rmtc_bot/src/strategy.cpp", ROOT / "tests/arena.cpp",
              ROOT / "tests/unstuck_benchmark.cpp", ROOT / "tests/run_unstuck.py"]
    report["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    for name, row in after.items():
        old = before[name] if before else None
        print(json.dumps({"case": name, "before_R": old["result"]["R"] if old else None, "after_R": row["result"]["R"],
                          "before_loop": old["longest_periodic_motion_without_score_progress"] if old else None,
                          "after_loop": row["longest_periodic_motion_without_score_progress"],
                          "first_task_tick": row["first_task_tick"], "invalid": row["result"]["invalid"]}, ensure_ascii=False))
    print("navigation regression passed; " + str(args.output))


if __name__ == "__main__":
    main()
