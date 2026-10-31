#!/usr/bin/env python3
"""验证场景工具的恢复、随机流、时序、输入隔离和失败产物；没有真实网络请求。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import run_scenarios

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None


class Scenarios(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 编译一次；各用例复用相同源码指纹对应的程序。
        run_scenarios.build(OPTIONS.json_include, OPTIONS.sanitize)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="rmtc-scenes-")
        self.work = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def launch(self, scene=None, resume=None, name="case", extra=()):
        command = [sys.executable, str(ROOT / "tests/run_scenarios.py"), "--out", str(self.work / name)]
        if OPTIONS.json_include:
            command += ["--json-include", OPTIONS.json_include]
        if OPTIONS.sanitize:
            command += ["--sanitize"]
        if resume:
            command += ["--resume", str(resume)]
        else:
            path = self.work / (name + ".scene.json")
            path.write_text(json.dumps(scene))
            command.append(str(path))
        completed = subprocess.run(command + list(extra), text=True, capture_output=True, timeout=150)
        path = self.work / (name + ".json")
        report = json.loads(path.read_text()) if path.exists() else None
        return completed, report

    def sample(self, name):
        return json.loads((ROOT / "tests/scenarios" / name).read_text())

    def passed(self, result):
        completed, report = result
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertTrue(report["passed"])
        return report

    def test_checked_in_examples(self):
        for path in sorted((ROOT / "tests/scenarios").glob("*.json")):
            with self.subTest(scene=path.name):
                self.passed(self.launch(json.loads(path.read_text()), name=path.stem))

    def test_warm_forks_copy_memory_and_rng(self):
        report = self.passed(self.launch(self.sample("warm_forks.json")))
        first, second = report["runs"]
        self.assertEqual(first["start_tick"], 3)
        self.assertEqual(first["mode"], "warm_prefix")
        self.assertEqual(first["world"], second["world"])
        self.assertEqual(first["trace"], second["trace"])
        # 文件恢复重放公开前缀；随机NPC与策略必须重现已保存动作，然后再跑同一后半段。
        checkpoint = self.work / "case.left.tick-5.checkpoint.json"
        continued = self.passed(self.launch(resume=checkpoint, name="continued", extra=("--until-tick", "8", "--clear-breakpoints")))
        self.assertEqual(continued["runs"][0]["world"], first["world"])
        self.assertEqual(continued["runs"][0]["decisions"], first["decisions"])

    def test_condition_breakpoint_is_before_next_action(self):
        scene = self.sample("defend_survives.json")
        scene["run"] = {"until_tick": 210, "break_when": [{"path": "/world/cars/A/hp", "op": "le", "value": 5}]}
        run = self.passed(self.launch(scene))["runs"][0]
        self.assertEqual((run["end_tick"], run["steps"], run["stop"]), (201, 1, "breakpoint"))
        self.assertTrue(run["world"]["cars"]["A"]["alive"])

    def test_world_roundtrip_keeps_effects_resources_and_respawn(self):
        scene = self.sample("attack_before_move.json")
        scene["initial"]["resources"] = [{"position": [4, 4], "value": 10}]
        scene["initial"]["cars"]["A"].update(hp=40, empowered=["fortress"], capture={"fortress": 2}, heal_until=204, heal_amount=5)
        scene["initial"]["cars"]["B"].update(hp=0, alive=False, respawn_in=2)
        for seat in "AB":
            scene["agents"][seat]["actions"] = {}
        scene["events"] = [{"id": "fortress", "notice_tick": 185, "delivery_tick": 185,
                            "event": {"kind": "fortress", "start": 190, "end": 220, "area": [4, 4, 4, 4],
                                      "amount": 35, "defended_damage": 7, "capture_ticks": 2, "source": "fixture"}}]
        scene["run"] = {"until_tick": 202, "save_at": [201]}
        full = self.passed(self.launch(scene))["runs"][0]
        self.assertEqual(full["world"]["cars"]["A"]["hp"], 50)
        self.assertEqual(full["world"]["cars"]["A"]["R"], 10)
        self.assertEqual(full["world"]["resources"], [])
        self.assertEqual(full["world"]["cars"]["A"]["empowered"], ["fortress"])
        self.assertTrue(full["world"]["cars"]["B"]["protected"])
        restored = self.passed(self.launch(resume=self.work / "case.main.tick-201.checkpoint.json", name="restored",
                                          extra=("--until-tick", "202")))["runs"][0]
        self.assertEqual(restored["world"], full["world"])

    def test_assertion_failure_keeps_checkpoint(self):
        scene = self.sample("defend_survives.json")
        scene["run"]["expect"][0]["value"] = 99
        completed, report = self.launch(scene)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertFalse(report["passed"])
        self.assertTrue((self.work / "case.main.checkpoint.json").exists())

    def test_rejects_inconsistent_world_and_misspelled_fields(self):
        original = self.sample("defend_survives.json")
        cases = [copy.deepcopy(original) for _ in range(4)]
        cases[0]["initial"]["cars"]["B"]["position"] = [4, 4]
        cases[1]["initial"]["tick"] = 200.5
        cases[2]["initial"]["cars"]["A"]["hhp"] = 15
        cases[3]["initial"]["cars"]["A"]["hp"] = -1
        for index, scene in enumerate(cases):
            with self.subTest(case=index):
                completed, _ = self.launch(scene, name="invalid" + str(index))
                self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)

    def test_rejects_edited_or_different_build_checkpoint(self):
        self.passed(self.launch(self.sample("defend_survives.json")))
        path = self.work / "case.main.checkpoint.json"
        changed = json.loads(path.read_text())
        changed["world"]["cars"]["A"]["hp"] = 100
        edited = self.work / "edited.json"
        edited.write_text(json.dumps(changed))
        completed, _ = self.launch(resume=edited, name="corrupt")
        self.assertEqual(completed.returncode, 2)
        valid = run_scenarios.read_bundle(path)
        valid["build_id"] = "a different compiled version"
        run_scenarios.save_bundle(edited, valid)
        completed, _ = self.launch(resume=edited, name="different")
        self.assertEqual(completed.returncode, 2)

    def test_no_notice_fork_does_not_remove_scoring_qualification(self):
        scene = self.sample("announcement_pair.json")
        scene["missions"] = [{"id": "past-task", "type": "resource_value", "target": 40}]
        scene["initial"]["cars"]["A"].update(R=40, normal=4, completed=["past-task"])
        report = self.passed(self.launch(scene))
        a, b = report["runs"]
        for run in report["runs"]:
            bundle = run_scenarios.read_bundle(run["checkpoint_file"])
            self.assertTrue(bundle["scene"]["qualified"])
            self.assertEqual(run["result"][0]["qualified_M"], 1)
        self.assertEqual(a["world"]["cars"]["A"]["hp"], 100)
        self.assertEqual(b["world"]["cars"]["A"]["hp"], 65)

    def test_export_edit_and_run_new_cold_scene(self):
        self.passed(self.launch(self.sample("defend_survives.json")))
        source = self.work / "case.main.checkpoint.json"
        target = self.work / "editable.scene.json"
        command = [sys.executable, str(ROOT / "tests/run_scenarios.py"), "--export-scene", str(source),
                   "--until-tick", "202", "--out", str(target)]
        subprocess.run(command, capture_output=True, text=True, check=True)
        scene = json.loads(target.read_text())
        self.assertEqual(scene["initial"]["tick"], 201)
        scene["initial"]["cars"]["A"]["hp"] = 100
        # 新摆局可以修改血量，但不能把这次新世界叫做原策略记忆的精确恢复。
        report = self.passed(self.launch(scene, name="edited_cold"))
        self.assertEqual(report["runs"][0]["mode"], "cold")

    def test_warm_fork_changes_only_future_enemy_action(self):
        scene = self.sample("defend_survives.json")
        scene["initial"]["tick"] = 198
        scene["agents"]["B"]["actions"] = {}
        scene["run"] = {"warm_until": 200, "until_tick": 201, "forks": [
            {"name": "attacks", "agents": {"B": {"actions": {"200": {"action": "attack", "direction": "left"}}}},
             "run": {"expect": [{"path": "/world/cars/A/hp", "op": "eq", "value": 5}]}},
            {"name": "defends", "agents": {"B": {"actions": {"200": {"action": "defend"}}}},
             "run": {"expect": [{"path": "/world/cars/A/hp", "op": "eq", "value": 15}]}}]}
        report = self.passed(self.launch(scene))
        first, second = [run_scenarios.read_bundle(r["checkpoint_file"]) for r in report["runs"]]
        self.assertEqual(first["tape"][:2], second["tape"][:2])
        self.assertNotEqual(first["tape"][2]["actions"]["B"], second["tape"][2]["actions"]["B"])
        self.passed(self.launch(resume=report["runs"][1]["checkpoint_file"], name="fork_resume", extra=("--until-tick", "202")))

    def test_pending_explanation_survives_checkpoint(self):
        scene = self.sample("announcement_pair.json")
        scene["events"][0]["delivery_tick"] = 201
        # 晚一Tick到达时只剩一步，已经不能在202爆炸前退到x<4；正确解释也可能赶不上。
        for branch in scene["run"]["forks"]:
            branch["run"]["expect"][0]["value"] = 65
        full = self.passed(self.launch(scene))["runs"][0]
        continued = self.passed(self.launch(resume=self.work / "case.notice.tick-201.checkpoint.json",
                                          name="late_resume", extra=("--until-tick", "203")))["runs"][0]
        self.assertEqual(continued["world"], full["world"])
        tape = run_scenarios.read_bundle(continued["checkpoint_file"])["tape"]
        self.assertEqual(tape[0]["event_ids"]["A"], [])
        self.assertEqual(tape[1]["event_ids"]["A"], ["blast"])

    def test_strategy_inputs_exclude_hidden_world(self):
        scene = self.sample("defend_survives.json")
        scene["map"].update(width=18, height=18, spawns=[[1, 1], [16, 1], [16, 16], [1, 16]])
        scene["initial"]["cars"] = {"A": {"position": [1, 1], "hp": 100}}
        scene["initial"]["resources"] = [{"position": [17, 17], "value": 20}]
        scene["agents"].pop("B")
        scene["run"]["expect"] = []
        run = self.passed(self.launch(scene))["runs"][0]
        bundle = run_scenarios.read_bundle(run["checkpoint_file"])
        observation = bundle["tape"][0]["perceptions"]["A"]
        self.assertEqual(observation["visible_entities"], [])
        self.assertEqual(observation["map"], {"width": 18, "height": 18})
        self.assertEqual(run["world"]["resources"], [{"position": [17, 17], "value": 20}])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-include")
    parser.add_argument("--sanitize", action="store_true")
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
