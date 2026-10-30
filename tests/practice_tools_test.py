#!/usr/bin/env python3
"""实测工具回归：补跑同图、逐局日志、异常回收与配置副本；不启动ROS/真实模型。"""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


batch = module("batch", "tools/batch_practice.py")
archive = module("archive", "tools/archive_batch.py")
seats = module("seats", "tools/build_seat_variants.py")
practice_league = module("practice_league", "tests/practice_version_league.py")


class PracticeTools(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rmtc-practice-tools-")
        self.work = Path(self.temporary.name)
        self.config = batch.base_config()

    def tearDown(self):
        self.temporary.cleanup()

    def run_batch(self, folder, only=None, valid=True):
        summary = self.work / folder / "summary.json"
        command = ["batch", "--count", "4", "--seed", "20261101", "--summary", str(summary)]
        if only is not None:
            command += ["--only", only]
        seen = []

        def battle(vid, cfg, path, logs, our):
            seen.append((vid, copy.deepcopy(cfg), logs))
            return dict(variant=vid, ok=True, session=f"case-{vid}")

        with patch.object(sys, "argv", command), patch.object(batch, "env_for_referee", return_value={}), \
             patch.object(batch, "seat_hashes", return_value={"A": {"binary_sha256": "fixture"}}), \
             patch.object(batch, "validate", return_value=(valid, "fixture rejected")), \
             patch.object(batch, "run_battle", side_effect=battle), contextlib.redirect_stdout(io.StringIO()):
            rc = batch.main()
        return rc, seen, json.loads(summary.read_text())

    def test_only_retry_keeps_exact_full_batch_map(self):
        _, full, _ = self.run_batch("full")
        rc, selected, _ = self.run_batch("only", "3")
        self.assertEqual(rc, 0)
        self.assertEqual(selected[0][1], full[3][1])
        self.assertNotEqual(selected[0][1], full[0][1])

    def test_per_match_logs_are_unique_and_completed_retry_is_cached(self):
        _, seen, results = self.run_batch("logs")
        self.assertEqual(len({e[2] for e in seen}), 4)
        for entry in results:
            self.assertTrue(Path(entry["config_path"]).is_file())
        rc, retried, again = self.run_batch("logs", "3")
        self.assertEqual((rc, retried, again), (0, [], results))

    def test_last_rejection_is_persisted_and_returns_failure(self):
        rc, seen, results = self.run_batch("reject", "3", valid=False)
        self.assertEqual((rc, seen, len(results)), (1, [], 1))
        self.assertFalse(results[0]["ok"])
        self.assertIn("rejected", results[0]["error"])

    def test_rotation_uses_seat_identity_not_robot_list_order(self):
        original = batch.apply_rotation(copy.deepcopy(self.config), 1)
        reordered = copy.deepcopy(self.config)
        reordered["robots"].reverse()
        actual = batch.apply_rotation(reordered, 1)
        self.assertEqual({r["team"]: r["spawn"] for r in original["robots"]},
                         {r["team"]: r["spawn"] for r in actual["robots"]})
        for invalid in (":install", "AB:install", "E:install", "A:"):
            with self.assertRaises(SystemExit):
                batch.parse_our(invalid)

    def test_dead_group_leader_still_receives_group_cleanup(self):
        process = SimpleNamespace(pid=12345, poll=lambda: 0, wait=Mock(return_value=0))
        with patch.object(batch.os, "killpg") as kill:
            batch.stop_battle([process])
        self.assertEqual([call.args[0] for call in kill.call_args_list], [12345, 12345])

    def test_missing_fourth_registration_fails_and_cleans_all_owned_processes(self):
        session = "practice-" + "1" * 32
        run = self.work / session
        run.mkdir()
        (run / "config.json").write_text(json.dumps(self.config))
        (run / "referee.log").write_text("registered team A\nregistered team B\nregistered team C\n")
        logs = self.work / "logs"
        logs.mkdir()
        state = dict(session_id=session, run_dir=str(run), settings=dict(config=self.config))
        ref = SimpleNamespace(pid=100, poll=lambda: None)
        bot = SimpleNamespace(pid=101, poll=lambda: None)

        def spawn(*args, **kwargs):
            kwargs["stdout"].write("练习裁判已就绪\n")
            kwargs["stdout"].flush()
            return ref

        with patch.dict(os.environ, {"RMTC_PRACTICE_PORT": "8080"}), \
             patch.object(batch, "env_for_referee", return_value={}), \
             patch.object(batch.subprocess, "Popen", side_effect=spawn), \
             patch.object(batch, "spawn_our_seat", return_value=bot), \
             patch.object(batch, "http_request", return_value=(200, state)), \
             patch.object(batch.time, "monotonic", side_effect=[0, 0, 91]), \
             patch.object(batch, "stop_battle") as stop, contextlib.redirect_stdout(io.StringIO()):
            result = batch.run_battle(3, self.config, "fixture.json", str(logs), {"A": "install"})
        self.assertFalse(result["ok"])
        self.assertIn("four seats", result["error"])
        self.assertEqual(stop.call_args.args, ([ref, bot], session))

    def test_foreign_service_config_cannot_be_claimed_or_stopped(self):
        run = self.work / ("practice-" + "2" * 32)
        run.mkdir()
        (run / "config.json").write_text(json.dumps(self.config))
        state = dict(session_id=run.name, run_dir=str(run), settings=dict(config={}))
        with self.assertRaisesRegex(ValueError, "不属于"):
            batch.owned_run(state, self.config, 0)
        with patch.object(batch, "http_request", return_value=(200, {"session_id": "someone-else"})), \
             patch.object(batch, "http_post") as stop:
            batch.stop_battle([], run.name)
        stop.assert_not_called()

    def test_archive_uses_each_sessions_log_and_keeps_failed_session(self):
        source = self.work / "raw"
        records = []
        for i in range(2):
            session = f"practice-{i}"
            (source / session).mkdir(parents=True)
            for piece in archive.PIECES:
                (source / session / piece).write_text("{}\n")
            logs = self.work / f"logs-{i}"
            logs.mkdir()
            (logs / "botA.log").write_text(f"session {i}")
            cfg = logs / f"config_{i}.json"
            cfg.write_text("{}")
            records.append(dict(variant=i, session=session, ok=i == 0, log_dir=str(logs), config_path=str(cfg), error="fixture" if i else None))
        summary = self.work / "summary.json"
        summary.write_text(json.dumps(records))
        target = self.work / "archive"
        with patch.object(sys, "argv", ["archive", "--summary", str(summary), "--to", str(target), "--source", str(source)]), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(archive.main(), 0)
        for i in range(2):
            self.assertEqual((target / f"practice-{i}/bot_A_ours.log").read_text(), f"session {i}")
        meta = json.loads((target / "archive.json").read_text())
        self.assertEqual((meta["completed"], meta["failed"], len(meta["sessions"])), (1, 1, 2))
        self.assertEqual(json.loads((target / "batch_summary.json").read_text()), records)

    def test_summary_file_is_scanned(self):
        path = self.work / "summary.json"
        path.write_text(json.dumps({"fixture": "sk-" + "x" * 24}))
        self.assertEqual(archive.scan_keys(str(path)), [str(path)])

    def test_seat_snapshot_never_changes_active_header_and_records_actual_config(self):
        source = self.work / "source"
        header = source / "src/rmtc_bot/include/rmtc_bot/match_config.hpp"
        header.parent.mkdir(parents=True)
        original = 'const char *team = "C";\nconst char *ai_token = "practice-C";\n'
        header.write_text(original)
        record = seats.snapshot(source, self.work / "frozen", "B")
        self.assertEqual(header.read_text(), original)
        patched = self.work / "frozen/src/rmtc_bot/include/rmtc_bot/match_config.hpp"
        self.assertIn('team = "B"', patched.read_text())
        self.assertIn('practice-B', patched.read_text())
        self.assertNotEqual(record["params_sha256"], record["original_params_sha256"])
        self.assertEqual(record["params_sha256"], seats.digest(patched))

    def test_failed_build_from_another_cwd_leaves_source_intact(self):
        source = self.work / "source"
        header = source / "src/rmtc_bot/include/rmtc_bot/match_config.hpp"
        header.parent.mkdir(parents=True)
        header.write_text('const char *team = "A";\nconst char *ai_token = "practice-A";\n')
        original = header.read_bytes()
        with patch.object(sys, "argv", ["build", str(source), "fixture", "B"]), \
             patch.object(seats.subprocess, "run", return_value=SimpleNamespace(returncode=7)) as build:
            with self.assertRaises(SystemExit) as error:
                seats.main()
        self.assertEqual(error.exception.code, 7)
        self.assertEqual(header.read_bytes(), original)
        self.assertEqual(build.call_args.kwargs["cwd"], source)
        # colcon 经 bash -c 调用（显式 source ROS）：整条命令串必须指向冻结副本路径。
        self.assertIn(str(source / "build-v-fixture-B/source/src"), build.call_args.args[0][2])

    def test_empty_practice_run_is_not_zero_of_zero_success(self):
        args = SimpleNamespace()
        with patch.object(practice_league.subprocess, "run", return_value=SimpleNamespace(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            rc, battles = practice_league.run_mix(1, "M1", {"A": "install"}, str(self.work), args)
        self.assertEqual((rc, battles), (1, []))

    def test_league_batch_exit_code_aggregates_failures(self):
        # 全完成 → 0；缺局/失败组/空计划 → 1（护栏有效但聚合退出码必须非零）。
        self.assertEqual(practice_league.batch_exit_code(1, 4, 4, 0), 0)
        self.assertEqual(practice_league.batch_exit_code(2, 8, 8, 0), 0)
        self.assertEqual(practice_league.batch_exit_code(1, 3, 4, 0), 1)   # 缺局
        self.assertEqual(practice_league.batch_exit_code(2, 8, 7, 0), 1)   # 返回数不足
        self.assertEqual(practice_league.batch_exit_code(2, 8, 8, 1), 1)   # 有失败组
        self.assertEqual(practice_league.batch_exit_code(0, 0, 0, 0), 1)   # 空计划

    def test_referee_ready_requires_new_line_per_attempt(self):
        # 每次尝试独立就绪基线：上一尝试残留的 Ready 行不能算本次就绪；
        # 日志被截断重开后，以本次新出现的就绪行为准。
        logf = self.work / "referee_stdout.log"
        logf.write_text("练习裁判已就绪\n")
        self.assertFalse(batch.referee_ready_since(self.work, 1))   # 旧行不算新就绪
        logf.write_text("练习裁判已就绪\n练习裁判已就绪\n")
        self.assertTrue(batch.referee_ready_since(self.work, 1))    # 本次新行
        logf.write_text("练习裁判已就绪\n")                         # 截断重开后再就绪
        self.assertTrue(batch.referee_ready_since(self.work, 3))
        logf.write_text("")                                         # 截断后尚未就绪
        self.assertFalse(batch.referee_ready_since(self.work, 3))


if __name__ == "__main__":
    unittest.main()
