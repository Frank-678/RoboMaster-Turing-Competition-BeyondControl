#!/usr/bin/env python3
"""Read-only/mocked probes for the DeepSeek eacb963 handoff findings.

No ROS, colcon, network requests or existing source modifications are made.
Results describe the supplied checkout, so retain its full commit identity.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch


def load(root, name, relative):
    spec = importlib.util.spec_from_file_location(name, root / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    root = args.source.resolve()
    batch = load(root, "review_batch", "tools/batch_practice.py")
    league = load(root, "review_league", "tests/practice_version_league.py")
    seats = load(root, "review_seats", "tools/build_seat_variants.py")
    a = {seat: league.MIXES["M1"].get(seat, "original") for seat in "BCD"}
    b = {seat: league.MIXES["M4"].get(seat, "original") for seat in "BCD"}
    findings = [{"id": "R1", "M1_opponents": a, "M4_opponents": b,
                 "same_opponent_pool": a == b, "issue_present": a != b}]
    with tempfile.TemporaryDirectory(prefix="rmtc-review-") as folder:
        base = Path(folder)
        log = base / "referee_stdout.log"
        log.write_text("old attempt\n练习裁判已就绪\n", encoding="utf-8")
        baseline = log.read_text().count("练习裁判已就绪")
        log.write_text("fresh attempt\n练习裁判已就绪\n", encoding="utf-8")
        actual = batch.referee_ready_since(base, baseline)
        findings.append({"id": "R2", "baseline": baseline, "new_ready_lines_after_truncate": 1,
                         "fresh_ready_expected": True, "actual": actual, "issue_present": not actual})
        source = base / "source with spaces"
        header = source / "src/rmtc_bot/include/rmtc_bot/match_config.hpp"
        header.parent.mkdir(parents=True)
        header.write_text('const char *team = "A"; const char *ai_token = "practice-A";\n')
        with patch.object(sys, "argv", ["build_seat_variants", str(source), "fixture", "B"]), \
             patch.object(seats.subprocess, "run", return_value=SimpleNamespace(returncode=7)) as invoked:
            try:
                seats.main()
            except SystemExit as exc:
                if exc.code != 7:
                    raise
        command = invoked.call_args.args[0]
        expected = str(source / "build-v-fixture-B/logs")
        preserves = command[:2] == ["bash", "-c"] and expected in shlex.split(command[2])
        findings.append({"id": "R3", "expected_one_argument": expected,
                         "shell_argument_preserved": preserves, "issue_present": not preserves,
                         "actual_shell_command": command})
    result = {"schema": 1, "scope": "read-only/mocked handoff audit; no real referee/build",
              "reviewed_commit": "eacb963dab4d38bf5f2c3112fb6043fe10279c61",
              "findings": findings}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({f["id"]: f["issue_present"] for f in findings}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
