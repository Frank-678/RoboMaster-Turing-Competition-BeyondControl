#!/usr/bin/env python3
"""假设火线与确定爆炸的32例回归；可只读比较旧工程，默认只测当前版本。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]  # 当前工程，夹具和裁判始终取这份。


def build(source_root, target, json_include):
    """source_root只提供生产核心；target是测试二进制，json_include是可选头文件目录。"""
    core = source_root / "src/rmtc_bot"
    flags = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "-I", str(core / "include"), "-I", str(ROOT / "tests")]
    if json_include:
        flags += ["-I", json_include]
    sources = [core / "src" / (name + ".cpp") for name in ("strategy", "tactics", "events")]
    sources += [ROOT / "tests/arena.cpp", ROOT / "tests/risk_benchmark.cpp"]
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(flags + [str(p) for p in sources] + ["-o", str(target)], check=True)
    return target


def run(binary, current):
    """读取完整32局；current只对新版要求保留逃路，旧版用于保留真实失败。"""
    completed = subprocess.run([str(binary.resolve())], text=True, capture_output=True, check=True, timeout=30)
    rows = [json.loads(line) for line in completed.stdout.splitlines()]
    assert len(rows) == 32
    assert {(r["seat"], r["rotation"], r["enemy_triggers_chili"]) for r in rows} == {
        (seat, rotation, trigger) for seat in range(4) for rotation in range(4) for trigger in (False, True)}
    for row in rows:
        assert row["result"]["invalid"] == 0, row
        if current:
            assert row["trace"][0]["action"] == {"action": "move", "direction": ["up", "right", "down", "left"][row["rotation"]]}, row
            assert row["result"]["deaths"] == int(row["enemy_triggers_chili"]), row
    return {"results": rows, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-include", help="非标准JSON头文件目录；已系统安装时不填")
    parser.add_argument("--baseline-root", type=Path, help="只读的旧工程目录")
    parser.add_argument("--before-binary", type=Path, help="已经编译的旧版同夹具二进制")
    parser.add_argument("--after-binary", type=Path, help="已经编译的当前同夹具二进制")
    parser.add_argument("--baseline-label", default="unspecified")
    parser.add_argument("--output", type=Path, default=ROOT / "build/risk/latest.json")
    args = parser.parse_args()
    after = args.after_binary or build(ROOT, ROOT / "build/risk/after", args.json_include)
    before = args.before_binary
    if args.baseline_root:
        before = build(args.baseline_root, ROOT / "build/risk/before", args.json_include)
    report = {"scope": "32 synthetic two-tick fixtures per version; hand-interpreted announced events, not original referee/DDS or real LLM",
              "baseline_code": args.baseline_label if before else None, "versions": {}}
    if before:
        report["versions"]["before"] = run(before, False)
    report["versions"]["after"] = run(after, True)
    paths = list((ROOT / "src/rmtc_bot").rglob("*.hpp")) + list((ROOT / "src/rmtc_bot/src").glob("*.cpp"))
    paths += [ROOT / "tests/arena.cpp", ROOT / "tests/risk_benchmark.cpp", Path(__file__)]
    report["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, separators=(",", ":")) + "\n")
    for version, data in report["versions"].items():
        print(version, {str(trigger): sum(row["result"]["deaths"] for row in data["results"]
                                          if row["enemy_triggers_chili"] == trigger) for trigger in (False, True)})
    print("32-case current regression passed: " + str(args.output))


if __name__ == "__main__":
    main()
