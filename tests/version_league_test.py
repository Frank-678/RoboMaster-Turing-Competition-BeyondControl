#!/usr/bin/env python3
"""联赛回归：先freeze一个版本，再指定--version；不启动ROS或付费模型。"""
import argparse
import copy
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import league_maps
import run_scenarios
import version_league as league
import version_store

OPTIONS = None


class League(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary, cls.manifest = version_store.load_version(OPTIONS.version)
        cls.engine, cls.engine_manifest = run_scenarios.build(OPTIONS.json_include)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rmtc-league-")
        self.directory = Path(self.temporary.name)
        self.versions = {"frozen": (self.binary, self.manifest)}
        self.plan = dict(anchor="frozen", candidates=["frozen"])

    def tearDown(self):
        self.temporary.cleanup()

    def case(self):
        scene = json.loads((league.ROOT / "tests/scenarios/defend_survives.json").read_text())
        scene["agents"]["A"] = dict(type="external", version="$focal", event_delivery="oracle")
        return dict(id="guard", map_id="small-guard", family="combat", mix="script", focal="A", scene=scene)

    def run_case(self, case, name="case"):
        return league.play(case, "frozen", self.plan, self.versions, self.engine, self.engine_manifest,
                           self.directory / name, 5)

    def test_arbitrary_tick_breakpoint_and_no_fabricated_wait(self):
        case = self.case()
        case["scene"]["run"] = dict(until_tick=210, break_when=[dict(path="/world/cars/A/hp", op="le", value=5)], save_at=[200])
        report = self.run_case(case)
        self.assertEqual(report["status"], "passed", report)
        self.assertEqual((report["start_tick"], report["end_tick"], report["stop"]), (200, 201, "breakpoint"))
        self.assertEqual(report["world"]["cars"]["A"]["hp"], 5)
        self.assertTrue((self.directory / "case/tick-200.world.json").exists())
        with gzip.open(self.directory / "case/tape.jsonl.gz", "rt") as stream:
            row = json.loads(next(stream))
        self.assertEqual(row["replies"]["A"]["action"], dict(action="defend"))

    def test_workers_receive_only_own_local_inputs(self):
        case = self.case()
        scene = case["scene"]
        scene["map"].update(width=18, height=18, spawns=[[1,1],[16,1],[16,16],[1,16]])
        scene["initial"]["cars"] = dict(A=dict(position=[1,1], hp=100))
        scene["initial"]["resources"] = [dict(position=[17,17], value=20)]
        scene["agents"].pop("B")
        scene["run"] = dict(until_tick=202)
        report = self.run_case(case)
        self.assertEqual(report["status"], "passed", report)
        with gzip.open(self.directory / "case/tape.jsonl.gz", "rt") as stream:
            row = json.loads(next(stream))
        sent = row["inputs"]["A"]
        self.assertEqual(set(sent), {"perception", "events", "qualified"})
        self.assertEqual(sent["perception"]["visible_entities"], [])
        self.assertEqual(report["world"]["resources"], scene["initial"]["resources"])

    def test_independent_frozen_process_for_each_seat(self):
        case = self.case()
        case["scene"]["agents"]["B"] = dict(type="external", version="frozen", event_delivery="none")
        case["scene"]["initial"]["cars"]["A"]["hp"] = 100
        case["scene"]["run"] = dict(until_tick=203)
        report = self.run_case(case)
        self.assertEqual(report["status"], "passed", report)
        with gzip.open(self.directory / "case/tape.jsonl.gz", "rt") as stream:
            rows = [json.loads(line) for line in stream]
        self.assertEqual([row["tick"] for row in rows], [200, 201, 202])
        for row in rows:
            self.assertEqual(row["inputs"]["A"]["perception"]["self"]["team"], "A")
            self.assertEqual(row["inputs"]["B"]["perception"]["self"]["team"], "B")
            self.assertEqual(set(row["replies"]), {"A", "B"})

    def test_assertion_failure_keeps_short_reproducible_scene(self):
        case = self.case()
        case["scene"]["run"]["expect"][0]["value"] = 99
        report = self.run_case(case)
        self.assertEqual(report["status"], "assertion_failed")
        cold = json.loads((self.directory / "case/failure.cold.scene.json").read_text())
        self.assertEqual(cold["initial"]["tick"], 201)
        self.assertEqual(cold["initial"]["cars"]["A"]["hp"], 5)

    def test_same_signature_cache_and_changed_scene_refusal(self):
        case = self.case()
        first = self.run_case(case)
        stamp = (self.directory / "case/tape.jsonl.gz").stat().st_mtime_ns
        self.assertEqual(first, self.run_case(case))
        self.assertEqual(stamp, (self.directory / "case/tape.jsonl.gz").stat().st_mtime_ns)
        case["scene"]["initial"]["cars"]["A"]["hp"] = 16
        with self.assertRaisesRegex(ValueError, "different sources/fixture"):
            self.run_case(case)

    def test_binary_source_and_manifest_tampering_rejected(self):
        copy_root = self.directory / "version"
        shutil.copytree(Path(OPTIONS.version), copy_root)
        binary = copy_root / "worker"
        original = binary.read_bytes()
        binary.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "binary changed"):
            version_store.load_version(copy_root)
        binary.write_bytes(original)
        header = copy_root / "source/src/rmtc_bot/include/rmtc_bot/match_config.hpp"
        header.write_bytes(header.read_bytes() + b"\n")
        with self.assertRaisesRegex(ValueError, "source changed"):
            version_store.load_version(copy_root)
        manifest = json.loads((copy_root / "manifest.json").read_text())
        manifest["provenance"]["commit"] = "0" * 40
        version_store.write_json(copy_root / "manifest.json", manifest)
        with self.assertRaisesRegex(ValueError, "manifest changed"):
            version_store.load_version(copy_root)

    def test_git_snapshot_does_not_read_dirty_worktree(self):
        repository = self.directory / "git"
        repository.mkdir()
        def git(*args):
            return subprocess.run(["git", "-C", str(repository), *args], capture_output=True, check=True).stdout.decode().strip()
        git("init")
        files = version_store.CPP + ["src/rmtc_bot/include/rmtc_bot/a.hpp"]
        for name in files:
            path = repository / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("committed\n")
        git("add", ".")
        git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "fixture")
        commit = git("rev-parse", "HEAD")
        (repository / files[0]).write_text("uncommitted change\n")
        sources, provenance = version_store.git_sources(repository, commit)
        self.assertEqual(provenance["commit"], commit)
        self.assertEqual(sources[files[0]], b"committed\n")

    def test_map_and_opponent_coverage_and_determinism(self):
        plan = league_maps.make_suite(["old", "new"], "old", dict(old="a", new="b"))
        self.assertEqual(len(plan["cases"]), 144)
        self.assertEqual(plan, league_maps.make_suite(["old", "new"], "old", dict(old="a", new="b")))
        self.assertEqual({case["focal"] for case in plan["cases"]}, set(SEATS := "ABCD"))
        self.assertEqual(len({case["map_id"] for case in plan["cases"]}), 12)
        scenes = {case["family"]: case for case in plan["cases"]}
        self.assertGreater(scenes["dead_ends"]["metrics"]["dead_ends"], 4)
        bridge = scenes["bridge"]["scene"]["map"]
        self.assertEqual([y for y in range(24) if [12, y] not in bridge["walls"]], [12])
        self.assertEqual(scenes["late_empty"]["scene"]["initial"]["resources"], [])
        self.assertEqual(scenes["late_empty"]["scene"]["initial"]["tick"], 200)
        for case in plan["cases"]:
            self.assertEqual(len(case["scene"]["agents"]), len(SEATS))
            self.assertEqual(case["metrics"]["resource_total"], 0 if case["family"] == "late_empty" else 200)

    def test_worker_crash_is_a_failure_without_substituted_action(self):
        broken = self.directory / "broken"
        broken.write_text("#!/usr/bin/env python3\nraise SystemExit(7)\n")
        broken.chmod(0o755)
        self.versions["frozen"] = broken, self.manifest
        report = self.run_case(self.case())
        self.assertEqual(report["status"], "failed")
        self.assertIn("exited", report["error"])
        self.assertNotIn("focal_result", report)

    def test_paired_statistics_group_seats_by_map(self):
        rows = [dict(candidate=version, case_id=str(index), map_id="one-map", status="passed", rank=1, win_share=1,
                     focal_result=dict(score=score, deaths=0))
                for index in range(4) for version, score in (("old", 30), ("new", 32))]
        result = league.summarize(rows, dict(candidates=["old", "new"]))
        self.assertEqual(result["paired"]["new"]["map_groups"], 1)
        self.assertEqual(result["paired"]["new"]["mean_map_delta"], 2)
        self.assertEqual(result["paired"]["new"]["paired_cases"], 4)

    def test_death_deducts_60_instead_of_clearing_all_damage(self):
        case = self.case()
        scene = case["scene"]
        scene["initial"]["cars"]["A"].update(hp=30, D=200)
        scene["agents"]["A"] = dict(type="script", event_delivery="none", otherwise=dict(action="defend"),
            actions={str(tick): dict(action="attack", direction="right") for tick in (200, 201)})
        scene["agents"]["B"]["actions"]["201"] = dict(action="attack", direction="left")
        scene["run"] = dict(until_tick=202)
        report = self.run_case(case)
        self.assertEqual(report["status"], "passed", report)
        self.assertEqual(report["world"]["cars"]["A"]["deaths"], 1)
        self.assertEqual(report["world"]["cars"]["A"]["D"], 180)  # 200+两次20-60，不是0。

    def test_lowhp_berserker_attacks_instead_of_shared_defense(self):
        case = self.case()
        case["scene"]["initial"]["cars"]["B"]["hp"] = 15
        case["scene"]["agents"]["B"] = dict(type="npc", style="berserker", event_delivery="none")
        engine = league.Lines([str(self.engine), "--serve"], self.directory / "npc.stderr", 5)
        try:
            engine.call(dict(scene=case["scene"], build_id=self.engine_manifest["build_id"]))
            state = engine.call(dict(op="step", actions=dict(A=dict(action="defend"))))
        finally:
            engine.close()
        self.assertEqual(state["decisions"]["B"]["chosen"], dict(action="attack", direction="left"))
        self.assertEqual(state["world"]["cars"]["A"]["hp"], 5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--json-include")
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
