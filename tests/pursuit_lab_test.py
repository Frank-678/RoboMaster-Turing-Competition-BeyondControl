#!/usr/bin/env python3
"""Analytical matrix cases, RMTC boundary cases and bounded-search contracts."""
import copy
import importlib.util
import math
from pathlib import Path
import random
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from pursuit_lab import DuelLab, DEFEND, OMIT, solve_matrix


def scene(width=5, height=3, positions=((1, 1), (3, 1)), hp=(100, 100), **changes):
    value = {"schema": 1, "complete_local_model": True,
             "width": width, "height": height,
             "walkable": [[x, y] for y in range(height) for x in range(width)],
             "players": [{"position": list(p), "hp": h} for p, h in zip(positions, hp)],
             "score_damage_cap": 300, "own_damage": 100,
             "baseline_action": {"action": "defend"},
             "search": {"horizon": 1, "joint_budget": 30000,
                        "time_budget_ms": 5000, "matrix_iterations": 2048}}
    value.update(changes)
    return value


class MatrixCases(unittest.TestCase):
    def test_pure_saddle(self):
        out = solve_matrix([[3, 2], [1, 0]])
        self.assertEqual(out.row, (1, 0))
        self.assertEqual(out.lower, 2)
        self.assertEqual(out.upper, 2)

    def test_matching_pennies_needs_mixing(self):
        out = solve_matrix([[1, -1], [-1, 1]], 10000)
        self.assertLessEqual(out.lower, 0)
        self.assertGreaterEqual(out.upper, 0)
        self.assertLess(out.upper - out.lower, 0.05)
        self.assertAlmostEqual(out.row[0], 0.5, delta=0.02)
        # Every fixed action has security -1; mixing improves its guarantee.
        self.assertGreater(out.lower, -0.05)

    def test_rock_paper_scissors(self):
        out = solve_matrix([[0, -1, 1], [1, 0, -1], [-1, 1, 0]], 10000)
        self.assertLessEqual(out.lower, 1e-12)
        self.assertGreaterEqual(out.upper, -1e-12)
        self.assertLess(out.upper - out.lower, 0.05)

    def test_random_2_by_2_bounds_enclose_exact_value(self):
        rng = random.Random(20261029)
        for _ in range(80):
            a, b, c, d = (rng.uniform(-9, 9) for _ in range(4))
            low, high = max(min(a, b), min(c, d)), min(max(a, c), max(b, d))
            if abs(low - high) < 1e-10:
                exact = low
            else:
                exact = (a * d - b * c) / (a - b - c + d)
            out = solve_matrix([[a, b], [c, d]], 128)
            self.assertLessEqual(out.lower, exact + 1e-10)
            self.assertGreaterEqual(out.upper, exact - 1e-10)
            self.assertAlmostEqual(sum(out.row), 1)
            self.assertAlmostEqual(sum(out.column), 1)

    def test_malformed_matrix_is_rejected(self):
        for matrix in ([], [[]], [[1], [1, 2]], [[math.nan]]):
            with self.assertRaises(ValueError):
                solve_matrix(matrix)


class SettlementCases(unittest.TestCase):
    def test_attack_before_movement(self):
        lab = DuelLab(scene(positions=((1, 1), (2, 1)), hp=(20, 100)))
        out = lab.step(lab.initial, (("move", 0), ("attack", 3)))
        self.assertEqual(out.hp[0], 0)
        self.assertEqual(out.positions[0], (1, 1))
        self.assertEqual(out.own_d, 40)  # 100 - 60, not a D reset.

    def test_defend_survives_current_attack(self):
        lab = DuelLab(scene(positions=((1, 1), (2, 1)), hp=(20, 100)))
        out = lab.step(lab.initial, (DEFEND, ("attack", 3)))
        self.assertEqual(out.hp[0], 10)
        self.assertEqual(out.own_d, 100)

    def test_mutual_kills_are_simultaneous(self):
        lab = DuelLab(scene(positions=((1, 1), (2, 1)), hp=(20, 20), own_damage=17))
        out = lab.step(lab.initial, (("attack", 1), ("attack", 3)))
        self.assertEqual(out.hp, (0, 0))
        self.assertEqual(out.own_d, 37)  # 17 + actual 20 + kill 60 - death 60.

    def test_swap_is_not_possible(self):
        lab = DuelLab(scene(positions=((1, 1), (2, 1))))
        out = lab.step(lab.initial, (("move", 1), ("move", 3)))
        self.assertEqual(out.positions, lab.initial.positions)

    def test_same_destination_blocks_both(self):
        lab = DuelLab(scene(positions=((0, 1), (2, 1))))
        out = lab.step(lab.initial, (("move", 1), ("move", 3)))
        self.assertEqual(out.positions, lab.initial.positions)

    def test_wall_does_not_block_normal_shot(self):
        raw = scene(positions=((0, 1), (2, 1)))
        raw["walkable"].remove([1, 1])
        lab = DuelLab(raw)
        out = lab.step(lab.initial, (("attack", 1), OMIT))
        self.assertEqual(out.hp[1], 80)
        self.assertEqual(out.own_d, 120)

    def test_defending_is_current_action_only(self):
        lab = DuelLab(scene(positions=((1, 1), (2, 1))))
        first = lab.step(lab.initial, (("attack", 1), DEFEND))
        out = lab.step(first, (("attack", 1), ("move", 0)))
        self.assertEqual(out.hp[1], 70)

    def test_resources_collected_by_survivors_and_removed_for_either_player(self):
        raw = scene(positions=((0, 1), (3, 1)), resources=[
            {"position": [1, 1], "value": 10}, {"position": [2, 1], "value": 20}])
        lab = DuelLab(raw)
        out = lab.step(lab.initial, (("move", 1), ("move", 3)))
        self.assertEqual(out.resources, ())
        self.assertEqual(out.own_r, 10)

    def test_score_cap_is_an_explicit_input(self):
        values = []
        for cap in (300, 500):
            lab = DuelLab(scene(positions=((1, 1), (2, 1)), own_damage=290, score_damage_cap=cap,
                                weights={"hp": 0, "death": 0, "goal": 0}))
            out = lab.step(lab.initial, (("attack", 1), OMIT))
            values.append(lab.value(out))
        self.assertAlmostEqual(values[0], 0.5)
        self.assertAlmostEqual(values[1], 0.6)

    def test_goal_distance_respects_walls(self):
        raw = scene(positions=((0, 1), (4, 2)), goal=[2, 1])
        raw["walkable"].remove([1, 1])
        lab = DuelLab(raw)
        self.assertEqual(lab.distances[(0, 1)], 4)  # Manhattan distance is only 2.

    def test_rotation_commutes_with_physical_transition(self):
        raw = scene(positions=((0, 1), (2, 1)), resources=[{"position": [0, 0], "value": 20}])
        raw["walkable"].remove([1, 1])
        actions = (("attack", 1), ("move", 0))
        lab = DuelLab(raw)
        expected = lab.step(lab.initial, actions)
        def rotate(p):
            return [raw["height"] - 1 - p[1], p[0]]
        changed = copy.deepcopy(raw)
        changed["width"], changed["height"] = raw["height"], raw["width"]
        changed["walkable"] = [rotate(p) for p in raw["walkable"]]
        for before, after in zip(raw["players"], changed["players"]):
            after["position"] = rotate(before["position"])
        changed["resources"][0]["position"] = rotate(raw["resources"][0]["position"])
        other = DuelLab(changed)
        actual = other.step(other.initial, tuple((a, (d + 1) % 4) for a, d in actions))
        self.assertEqual(actual.positions, tuple(tuple(rotate(p)) for p in expected.positions))
        self.assertEqual(actual.hp, expected.hp)
        self.assertEqual(actual.own_d, expected.own_d)


class DecisionCases(unittest.TestCase):
    def test_shoot_corner_target_before_it_moves(self):
        raw = scene(width=3, height=2, positions=((2, 1), (2, 0)), hp=(100, 10),
                    own_damage=0, baseline_action={"action": "move", "direction": "left"})
        out = DuelLab(raw).search()
        self.assertEqual(out["completed_depth"], 1)
        self.assertEqual(out["sampled_action"], {"action": "attack", "direction": "up"})
        self.assertEqual(out["recommendation"], "candidate_exceeds_baseline_in_model")

    def test_move_cannot_save_low_hp_but_defend_can(self):
        raw = scene(positions=((1, 1), (2, 1)), hp=(20, 100),
                    baseline_action={"action": "move", "direction": "up"})
        out = DuelLab(raw).search()
        self.assertEqual(out["sampled_action"], {"action": "defend"})
        self.assertGreater(out["certificate"]["lower"], out["certificate"]["baseline_upper"])

    def test_finite_node_budget_returns_explicit_baseline(self):
        raw = scene()
        raw["search"]["joint_budget"] = 1
        out = DuelLab(raw).search()
        self.assertEqual(out["completed_depth"], 0)
        self.assertIsNone(out["certificate"])
        self.assertEqual(out["sampled_action"], raw["baseline_action"])

    def test_partial_deeper_matrix_does_not_replace_completed_depth(self):
        one = DuelLab(scene()).search()
        raw = scene()
        raw["search"].update(horizon=3, joint_budget=150)
        out = DuelLab(raw).search()
        self.assertEqual(out["completed_depth"], 1)
        self.assertTrue(out["budget_exhausted"])
        self.assertEqual(out["lower_matrix"], one["lower_matrix"])
        self.assertEqual(out["certificate"], one["certificate"])

    def test_two_step_search_reports_recursive_bounds(self):
        raw = scene(width=3, height=2, positions=((0, 0), (2, 1)),
                    resources=[{"position": [1, 0], "value": 20}], goal=[1, 0])
        raw["search"].update(horizon=2)
        out = DuelLab(raw).search()
        self.assertEqual(out["completed_depth"], 2)
        self.assertLessEqual(out["certificate"]["lower"], out["certificate"]["upper"] + 1e-10)
        self.assertAlmostEqual(sum(p["probability"] for p in out["policy"]), 1)

    def test_unsupported_information_is_not_silently_accepted(self):
        for change in ({"complete_local_model": False}, {"events": []}, {"enemy_score": 55},
                       {"score_damage_cap": float("inf")}, {"players": []}):
            raw = scene(**change)
            with self.assertRaises((ValueError, KeyError)):
                DuelLab(raw)
        raw = scene()
        del raw["score_damage_cap"]
        with self.assertRaises(KeyError):
            DuelLab(raw)

    def test_seed_replays_the_distribution_sample(self):
        raw = scene(width=3, height=2, positions=((0, 0), (2, 1)))
        first, second = DuelLab(raw).search(19), DuelLab(raw).search(19)
        for key in ("policy", "sampled_action", "certificate", "lower_matrix"):
            self.assertEqual(first[key], second[key])


if __name__ == "__main__":
    unittest.main(verbosity=2)
