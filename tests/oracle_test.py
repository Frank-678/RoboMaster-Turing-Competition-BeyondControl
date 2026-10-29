#!/usr/bin/env python3
"""短段最优参照：枚举完整性、截断诚实性、动作权限、复制隔离与目标取舍。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import run_scenarios

OPTIONS = None
ROOT = run_scenarios.ROOT


class Oracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        run_scenarios.build(OPTIONS.json_include)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rmtc-oracle-")
        self.work = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def scene(self):
        scene = json.loads((ROOT / "tests/scenarios/defend_survives.json").read_text())
        scene["oracle"] = dict(seat="A", depth=1, max_nodes=10000, include_no_submission=False, objective=[
            dict(path="/world/cars/A/deaths", sense="min"), dict(path="/world/cars/A/hp", sense="max")])
        return scene

    def launch(self, scene, name="case"):
        source, prefix = self.work / (name + ".json"), self.work / name
        source.write_text(json.dumps(scene))
        command = [sys.executable, str(ROOT / "tests/run_scenarios.py"), str(source), "--oracle", "--out", str(prefix)]
        if OPTIONS.json_include: command += ["--json-include", OPTIONS.json_include]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        report = Path(str(prefix) + ".oracle.json")
        return result, json.loads(report.read_text()) if report.exists() else None

    def test_one_tick_exact_best_defense_and_unchanged_root(self):
        result, report = self.launch(self.scene())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report["complete"])
        self.assertTrue(report["optimal_for_stated_fixture"])
        self.assertEqual(len(report["root_actions"]), 9)
        self.assertEqual(report["best_sequence"], [dict(action="defend")])
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["hp"], 5)
        self.assertEqual(report["initial"]["world"]["cars"]["A"]["hp"], 15)

    def test_early_move_survives_but_late_move_does_not(self):
        scene = self.scene()
        scene["initial"]["cars"]["A"]["hp"] = 30
        scene["oracle"]["depth"] = 3
        scene["agents"]["B"]["actions"] = {str(t): dict(action="attack", direction="left") for t in range(200, 203)}
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(report["complete"])
        self.assertIn(report["best_sequence"][0], [dict(action="move", direction="up"), dict(action="move", direction="down")])
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["deaths"], 0)
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["hp"], 10)
        defend = next(row for row in report["root_actions"] if row["action"] == dict(action="defend"))
        self.assertTrue(defend["subtree_complete"])
        self.assertEqual(defend["best_state"]["world"]["cars"]["A"]["deaths"], 1)

    def test_budget_cut_never_claims_optimal_or_fabricates_leaf(self):
        scene = self.scene()
        scene["oracle"].update(depth=4, max_nodes=1)
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(report["complete"])
        self.assertFalse(report["optimal_for_stated_fixture"])
        self.assertIsNone(report["best_sequence"])
        self.assertEqual(report["nodes"], 1)
        self.assertEqual(report["leaves"], 0)

    def test_protected_spawn_never_generates_attack(self):
        scene = self.scene()
        scene["initial"]["cars"]["A"].update(hp=100, protected=True, protected_left=2)
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(report["root_actions"]), 5)
        self.assertFalse(any(row["action"]["action"] == "attack" for row in report["root_actions"]))

    def test_two_resource_plan_exactly_beats_nearest_greedy(self):
        scene = self.scene()
        scene["stage"] = 1
        scene["initial"] = dict(tick=0, cars=dict(A=dict(position=[1,1], hp=100)), resources=[
            dict(position=[2,1], value=10), dict(position=[1,2], value=20), dict(position=[1,3], value=10)])
        scene["agents"].pop("B")
        scene["oracle"] = dict(seat="A", depth=2, max_nodes=1000, include_no_submission=False,
                               objective=[dict(path="/world/cars/A/R", sense="max")])
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(report["root_actions"]), 4)
        self.assertEqual(report["best_sequence"], [dict(action="move", direction="down")] * 2)
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["R"], 30)
        right = next(row for row in report["root_actions"] if row["action"]["direction"] == "right")
        self.assertEqual(right["best_state"]["world"]["cars"]["A"]["R"], 10)

    def test_explicit_damage_objective_can_choose_mutual_kill(self):
        scene = self.scene()
        scene["initial"]["cars"]["B"]["hp"] = 15
        scene["oracle"]["objective"] = [dict(path="/world/cars/A/D", sense="max")]
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report["best_sequence"], [dict(action="attack", direction="right")])
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["D"], 15)
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["deaths"], 1)

    def test_clock_budget_opponent_is_not_labeled_exact(self):
        scene = self.scene()
        scene["agents"]["B"] = dict(type="strategy", event_delivery="oracle")
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 2)
        self.assertIn("exact oracle opponents", result.stderr)
        self.assertIsNone(report)

    def test_invalid_objective_is_rejected_before_search(self):
        scene = self.scene()
        scene["oracle"]["objective"][0]["path"] = "/world/cars/A/misspelled"
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 2)
        self.assertIsNone(report)

    def test_cloned_npc_rng_is_repeatable(self):
        scene = self.scene()
        scene["initial"]["cars"]["A"]["hp"] = 100
        scene["agents"]["B"] = dict(type="npc", style="wanderer", event_delivery="none")
        scene["oracle"]["depth"] = 2
        first, a = self.launch(scene, "first")
        second, b = self.launch(copy.deepcopy(scene), "second")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        for key in ("best_sequence", "best_state", "root_actions", "nodes", "leaves"):
            self.assertEqual(a[key], b[key])

    def test_explicit_no_submission_can_complete_stage1_action_set(self):
        scene = self.scene()
        scene["stage"] = 1
        scene["initial"]["cars"].pop("B")
        scene["initial"]["resources"] = [dict(position=[4,4], value=20)]
        scene["agents"].pop("B")
        scene["oracle"].update(include_no_submission=True, objective=[dict(path="/world/cars/A/R", sense="max")])
        result, report = self.launch(scene)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(report["root_actions"]), 5)
        self.assertEqual(report["best_sequence"], [None])
        self.assertEqual(report["best_state"]["world"]["cars"]["A"]["R"], 20)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-include")
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
