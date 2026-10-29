#!/usr/bin/env python3
"""离线短场景：JSON摆局、条件断点、分叉和回放恢复；不启动ROS或调用模型。"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["src/rmtc_bot/src/strategy.cpp", "src/rmtc_bot/src/tactics.cpp", "src/rmtc_bot/src/events.cpp",
           "tests/arena.cpp", "tests/arena_checkpoint.cpp", "tests/arena_opponents.cpp", "tests/scenario_runner.cpp"]


def digest(value):
    """文件回放包的校验值：只防误改，不把校验值当作安全签名。"""
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def build(include=None, sanitize=False):
    """按源码/头文件/编译器/选项缓存；配置改动后使用另一个独立二进制，不复用旧构建。"""
    compiler = subprocess.run(["g++", "--version"], text=True, capture_output=True, check=True).stdout.splitlines()[0]
    flags = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "-I", str(ROOT / "src/rmtc_bot/include"), "-I", str(ROOT / "tests")]
    if include:
        flags += ["-I", str(Path(include).resolve())]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    paths = SOURCES + [str(p.relative_to(ROOT)) for p in sorted((ROOT / "src/rmtc_bot/include").rglob("*.hpp"))] + ["tests/arena.hpp"]
    hashes = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths}
    manifest = {"compiler": compiler, "flags": flags, "source_sha256": hashes}
    if include:
        header = Path(include) / "nlohmann/json.hpp"
        manifest["json_header_sha256"] = hashlib.sha256(header.read_bytes()).hexdigest()
    identity = digest(manifest)
    destination = ROOT / "build/scenarios" / identity
    destination.mkdir(parents=True, exist_ok=True)
    binary = destination / "runner"
    if not binary.exists():
        temporary = destination / ("runner." + str(os.getpid()) + ".new")
        subprocess.run(["g++", *flags, *[str(ROOT / p) for p in SOURCES], "-o", str(temporary)], check=True)
        temporary.replace(binary)
    manifest["build_id"] = identity
    manifest["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    return binary, manifest


def read_bundle(path):
    """误改世界或历史输入就报错；新构建也会被C++入口拒绝，不能冒充原程序恢复。"""
    value = json.loads(Path(path).read_text())
    expected = value.pop("_integrity_sha256")
    if digest(value) != expected:
        raise ValueError("回放包已被改动；请用场景JSON另开冷启动测试")
    return value


def save_bundle(path, value):
    wrapped = {**value, "_integrity_sha256": digest(value)}
    path.write_text(json.dumps(wrapped, ensure_ascii=False, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", nargs="?", help="场景JSON，或使用--resume")
    parser.add_argument("--resume", help="由本工具保存的checkpoint.json")
    parser.add_argument("--export-scene", help="把回放包的世界另存为可修改的冷启动场景；--out在此模式是场景文件名")
    parser.add_argument("--until-tick", type=int, help="下一次停止的绝对Tick；不执行该Tick动作")
    parser.add_argument("--clear-breakpoints", action="store_true", help="恢复时移除仍然成立的条件断点")
    parser.add_argument("--out", required=True, help="报告文件名前缀，例如build/checks/duel")
    parser.add_argument("--json-include", help="仅用于非标准测试环境；Ubuntu安装nlohmann-json3-dev后不用填")
    parser.add_argument("--sanitize", action="store_true", help="独立构建ASan/UBSan版本")
    args = parser.parse_args()
    if sum(bool(x) for x in (args.scene, args.resume, args.export_scene)) != 1:
        parser.error("请三选一：场景JSON、--resume 回放包、--export-scene 回放包")
    if args.export_scene:
        bundle = read_bundle(args.export_scene)
        scene = copy.deepcopy(bundle["scene"])
        world = bundle["world"]
        scene["name"] += "-cold-" + str(world["tick"])
        scene["initial"] = {"tick": world["tick"], "resources": world["resources"],
                            "cars": {seat: world["cars"][seat] for seat in scene["agents"]}}
        scene["run"].pop("forks", None)
        scene["run"].pop("warm_until", None)
        if args.until_tick is not None:
            scene["run"]["until_tick"] = args.until_tick
            scene["run"]["expect"] = []
        if args.clear_breakpoints:
            scene["run"]["break_when"] = []
        destination = Path(args.out)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps({"cold_scene": str(destination), "tick": world["tick"], "policy_memory": "reset"}, ensure_ascii=False))
        return 0
    binary, manifest = build(args.json_include, args.sanitize)
    job = {"build_id": manifest["build_id"]}
    if args.resume:
        job["resume"] = read_bundle(args.resume)
        control = dict(job["resume"]["scene"]["run"])
        # 已选定一个分支的恢复包不能重新启动原始fork列表或再次回到旧warm_until。
        control.pop("forks", None)
        control.pop("warm_until", None)
    else:
        job["scene"] = json.loads(Path(args.scene).read_text())
        control = dict(job["scene"]["run"])
    if args.until_tick is not None:
        control["until_tick"] = args.until_tick
        control["expect"] = []  # 原终点断言不自动移到新终点，持续不变量仍保留。
    if args.clear_breakpoints:
        control["break_when"] = []
    job["run"] = control
    completed = subprocess.run([str(binary)], input=json.dumps(job, ensure_ascii=False),
                               text=True, capture_output=True, timeout=120)
    if completed.returncode not in (0, 1):
        raise RuntimeError(completed.stderr.strip() or "scenario runner failed")
    report = json.loads(completed.stdout)
    prefix = Path(args.out)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    for run in report["runs"]:
        name = run["name"]
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise ValueError("fork名称仅允许字母、数字、下划线、减号")
        stem = Path(str(prefix) + "." + name)
        checkpoint = run.pop("checkpoint")
        save_bundle(Path(str(stem) + ".checkpoint.json"), checkpoint)
        for tick, bundle in run.pop("saved").items():
            save_bundle(Path(str(stem) + ".tick-" + tick + ".checkpoint.json"), bundle)
        run["checkpoint_file"] = str(stem) + ".checkpoint.json"
    report["build"] = manifest
    Path(str(prefix) + ".json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"report": str(prefix) + ".json", "passed": report["passed"],
                      "runs": [{k: r[k] for k in ("name", "start_tick", "end_tick", "steps", "stop", "passed")}
                               for r in report["runs"]]}, ensure_ascii=False))
    return completed.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("场景测试失败：" + str(error), file=sys.stderr)
        sys.exit(2)
