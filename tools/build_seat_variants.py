#!/usr/bin/env python3
"""把指定源码复制后改席位再编译，保留实际配置/源码/二进制摘要；从不改活跃源码。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(source, destination, seat):
    """完整复制ROS包源码；只在副本改team和公开practice令牌，不转写正式令牌。"""
    shutil.copytree(source / "src", destination / "src")
    # CMakeLists 在 BUILD_TESTING=ON（默认）下引用 ../../tests/*.cpp → 冻结树也带 tests（存在时）。
    if (source / "tests").is_dir():
        shutil.copytree(source / "tests", destination / "tests")
    header = destination / "src/rmtc_bot/include/rmtc_bot/match_config.hpp"
    original = digest(header)
    text, teams = re.subn(r'(\bteam\s*=\s*")[^"]*(")', lambda m: m[1] + seat + m[2], header.read_text())
    text, tokens = re.subn(r'(\bai_token\s*=\s*")practice-[A-D](")', lambda m: m[1] + "practice-" + seat + m[2], text)
    if teams != 1 or tokens != 1:
        raise ValueError("仅支持明确的team和公开practice令牌；请在测试源码副本中配置后重试")
    header.write_text(text)
    sources = {str(p.relative_to(destination)): digest(p) for p in sorted((destination / "src").rglob("*")) if p.is_file()}
    return {"original_params_sha256": original, "params_sha256": digest(header), "sources": sources}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=Path)
    parser.add_argument("version_name")
    parser.add_argument("seats")
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    source = args.source_dir.resolve()
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", args.version_name):
        parser.error("version_name只能包含字母、数字、下划线和减号")
    seats = args.seats.split(",")
    if not seats or any(seat not in set("ABCD") for seat in seats) or len(set(seats)) != len(seats):
        parser.error("seats须为A/B/C/D的不重复逗号列表")
    for seat in seats:
        build = source / f"build-v-{args.version_name}-{seat}"
        install = source / f"install-v-{args.version_name}-{seat}"
        if build.exists() or install.exists():
            raise SystemExit(f"版本目录已经存在，未覆盖：{build.name}；请选新版本名")
        frozen = build / "source"
        frozen.mkdir(parents=True)
        record = snapshot(source, frozen, seat)
        # base-paths和cwd都明确，不能从调用者的另一个工作空间误找源码；
        # colcon 需要 ROS 环境（login shell 不保证 source，显式 source）。
        # 固定 ROS 初始化与真实 colcon 参数分开：参数走数组原样传递，
        # 含空格路径不再经过 bash -c 字符串词法拆分（review_handoff_probe R3）。
        completed = subprocess.run(
            ["bash", "-c", 'source /opt/ros/humble/setup.bash && exec "$@"',
             "rmtc-seat-build",
             "colcon", "--log-base", str(build / "logs"),
             "build", "--base-paths", str(frozen / "src"),
             "--packages-select", "rmtc_bot", "--build-base", str(build / "packages"),
             "--install-base", str(install)], cwd=source)
        if completed.returncode:
            raise SystemExit(completed.returncode)
        binary = install / "rmtc_bot/lib/rmtc_bot/bot"
        record.update(install_base=str(install), binary_sha256=digest(binary))
        (build / "frozen.json").write_text(json.dumps(record, ensure_ascii=False, indent=2))
        if args.manifest:
            path = args.manifest.resolve()
            data = json.loads(path.read_text()) if path.exists() else {}
            data.setdefault("seats", {}).setdefault(args.version_name, {})[seat] = {
                key: record[key] for key in ("install_base", "binary_sha256", "params_sha256", "original_params_sha256")}
            data["seats"][args.version_name][seat]["source_manifest"] = str(build / "frozen.json")
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2))
            temporary.replace(path)
        print(f"seat={seat} frozen={build / 'frozen.json'} binary_sha256={record['binary_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
