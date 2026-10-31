#!/usr/bin/env python3
"""Offline, bounded simultaneous-move duel laboratory. Not imported by the bot.

The model is deliberately explicit: two initially alive ordinary fighters, a
closed known board, no events/missions/respawns during a horizon of at most 3.
All reported bounds concern this finite surrogate game, not a real RMTC match.
Only the Python standard library is required. No external code was copied.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import sys
import time
from collections import deque
from dataclasses import dataclass, replace
from pathlib import Path

DIRS = ((0, -1), (1, 0), (0, 1), (-1, 0))
NAMES = ("up", "right", "down", "left")
DEFEND = ("defend", -1)
OMIT = ("omit", -1)


def action_json(action):
    if action == OMIT:
        return None
    out = {"action": action[0]}
    if action[0] in ("move", "attack"):
        out["direction"] = NAMES[action[1]]
    return out


def parse_action(value):
    if value is None:
        return OMIT
    if not isinstance(value, dict):
        raise ValueError("action must be an object or null")
    kind = value.get("action")
    if kind == "defend" and set(value) == {"action"}:
        return DEFEND
    if kind in ("move", "attack") and set(value) == {"action", "direction"}:
        if value["direction"] in NAMES:
            return kind, NAMES.index(value["direction"])
    raise ValueError("invalid action")


def finite(value, label, low=0.0, high=1e9):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label} must be a number")
    value = float(value)
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{label} outside [{low}, {high}]")
    return value


def integer(value, label, low, high):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ValueError(f"{label} must be an integer in [{low}, {high}]")
    return value


def cell(value):
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError("cell must be [x,y]")
    return tuple(integer(x, "coordinate", 0, 63) for x in value)


def ray(start, target):
    dx, dy = target[0] - start[0], target[1] - start[1]
    if dx == 0 and 1 <= abs(dy) <= 2:
        return 2 if dy > 0 else 0
    if dy == 0 and 1 <= abs(dx) <= 2:
        return 1 if dx > 0 else 3
    return -1


@dataclass(frozen=True)
class State:
    positions: tuple
    hp: tuple
    own_r: float
    own_d: float
    resources: tuple  # sorted ((x,y), value) entries
    own_deaths: int = 0


@dataclass(frozen=True)
class MatrixSolution:
    row: tuple
    column: tuple
    lower: float
    upper: float


def solve_matrix(matrix, iterations=512, check_budget=None):
    """Fictitious play + directly checked weak-duality certificate.

    The empirical policies are candidates, not exact Nash claims. For ANY row
    distribution x and column distribution y, min_j x^T A[:,j] <= v(A) <=
    max_i A[i,:] y. Those quantities are computed from the complete matrix.
    """
    m, n = len(matrix), len(matrix[0]) if matrix else 0
    if not m or not n or any(len(row) != n for row in matrix):
        raise ValueError("matrix must be nonempty and rectangular")
    if any(not math.isfinite(a) for row in matrix for a in row):
        raise ValueError("matrix must be finite")
    if iterations < 1:
        raise ValueError("iterations must be positive")
    row_min = [min(row) for row in matrix]
    col_max = [max(matrix[i][j] for i in range(m)) for j in range(n)]
    i0 = max(range(m), key=row_min.__getitem__)
    j0 = min(range(n), key=col_max.__getitem__)
    if col_max[j0] - row_min[i0] <= 1e-12:
        return MatrixSolution(tuple(float(i == i0) for i in range(m)),
                              tuple(float(j == j0) for j in range(n)),
                              row_min[i0], col_max[j0])
    rc, cc = [1.0] * m, [1.0] * n
    # Cumulative payoff against the opponent's empirical action counts.
    row_payoff = [sum(row) for row in matrix]
    col_payoff = [sum(matrix[i][j] for i in range(m)) for j in range(n)]
    for iteration in range(iterations):
        if check_budget is not None and iteration % 64 == 0:
            check_budget()
        i = max(range(m), key=row_payoff.__getitem__)
        j = min(range(n), key=col_payoff.__getitem__)
        rc[i] += 1.0
        cc[j] += 1.0
        for k in range(m):
            row_payoff[k] += matrix[k][j]
        for k in range(n):
            col_payoff[k] += matrix[i][k]
    x, y = tuple(a / sum(rc) for a in rc), tuple(a / sum(cc) for a in cc)
    lower = min(sum(x[i] * matrix[i][j] for i in range(m)) for j in range(n))
    upper = max(sum(matrix[i][j] * y[j] for j in range(n)) for i in range(m))
    return MatrixSolution(x, y, lower, upper)


class BudgetExceeded(Exception):
    pass


class DuelLab:
    def __init__(self, scene):
        allowed = {"schema", "id", "complete_local_model", "width", "height",
                   "walkable", "players", "own_resource", "own_damage", "resources",
                   "score_damage_cap", "baseline_action", "goal", "weights", "search"}
        if not isinstance(scene, dict) or set(scene) - allowed:
            raise ValueError("unsupported fields; no events, missions, other scores or effects in v1")
        if scene.get("schema") != 1 or scene.get("complete_local_model") is not True:
            raise ValueError("requires schema=1 and explicit complete_local_model=true (offline only)")
        self.width = integer(scene["width"], "width", 1, 64)
        self.height = integer(scene["height"], "height", 1, 64)
        self.walkable = frozenset(cell(x) for x in scene["walkable"])
        if not self.walkable or any(x >= self.width or y >= self.height for x, y in self.walkable):
            raise ValueError("walkable cells must be inside the board")
        players = scene["players"]
        if not isinstance(players, list) or len(players) != 2:
            raise ValueError("v1 supports exactly two initially alive ordinary players")
        if any(not isinstance(p, dict) or set(p) != {"position", "hp"} for p in players):
            raise ValueError("players require position and hp only; effects are unsupported")
        positions = tuple(cell(p["position"]) for p in players)
        hp = tuple(finite(p["hp"], "hp", 1e-9, 100) for p in players)
        if len(set(positions)) != 2 or any(p not in self.walkable for p in positions):
            raise ValueError("players require distinct walkable positions")
        resources = []
        for r in scene.get("resources", []):
            if not isinstance(r, dict) or set(r) != {"position", "value"}:
                raise ValueError("resources require position and value")
            p = cell(r["position"])
            if p not in self.walkable:
                raise ValueError("resource outside walkable cells")
            resources.append((p, finite(r["value"], "resource value", 1e-9)))
        if len({p for p, _ in resources}) != len(resources):
            raise ValueError("duplicate resource")
        self.initial = State(positions, hp, finite(scene.get("own_resource", 0), "own_resource"),
                             finite(scene.get("own_damage", 0), "own_damage"), tuple(sorted(resources)))
        # Deliberately required: repository rule texts and old evaluator use
        # different damage denominators. The caller must pin the version.
        self.damage_cap = finite(scene["score_damage_cap"], "score_damage_cap", 1)
        self.baseline = parse_action(scene["baseline_action"])
        if self.baseline not in self.choices(self.initial, 0):
            raise ValueError("baseline is outside the modeled legal action set")
        w = scene.get("weights", {})
        if set(w) - {"hp", "death", "goal", "override_margin"}:
            raise ValueError("unknown weight")
        self.weights = {k: finite(w.get(k, v), k) for k, v in
                        {"hp": 0.03, "death": 3.0, "goal": 1.0, "override_margin": 0.05}.items()}
        self.goal = cell(scene["goal"]) if scene.get("goal") is not None else None
        if self.goal is not None and self.goal not in self.walkable:
            raise ValueError("goal must be known walkable")
        self.distances = self.bfs(self.goal) if self.goal is not None else {}
        if self.goal is not None and positions[0] not in self.distances:
            raise ValueError("goal must be reachable in the known movement graph")
        options = scene.get("search", {})
        if set(options) - {"horizon", "joint_budget", "time_budget_ms", "matrix_iterations"}:
            raise ValueError("unknown search option")
        self.horizon = integer(options.get("horizon", 2), "horizon", 1, 3)
        self.joint_budget = integer(options.get("joint_budget", 30000), "joint_budget", 1, 1000000)
        self.time_ms = finite(options.get("time_budget_ms", 200), "time_budget_ms", 0.001, 60000)
        self.iterations = integer(options.get("matrix_iterations", 512), "matrix_iterations", 1, 100000)
        self.transitions = 0
        self.cache = {}
        self.deadline = math.inf

    def bfs(self, source):
        out, queue = {source: 0}, deque([source])
        while queue:
            p = queue.popleft()
            for dx, dy in DIRS:
                q = p[0] + dx, p[1] + dy
                if q in self.walkable and q not in out:
                    out[q] = out[p] + 1
                    queue.append(q)
        return out

    def choices(self, state, player):
        if state.hp[player] <= 0:
            return (OMIT,)
        p = state.positions[player]
        occupied = {state.positions[i] for i in range(2) if state.hp[i] > 0}
        # Include every attack direction, even when redundant.
        # A prediction never deletes an adversary's legal ATTACK.
        actions = [DEFEND] + [("attack", d) for d in range(4)]
        actions += [("move", d) for d, (dx, dy) in enumerate(DIRS)
                    if (p[0] + dx, p[1] + dy) in self.walkable
                    and (p[0] + dx, p[1] + dy) not in occupied]
        # For an ordinary living focal car DEFEND weakly dominates omission;
        # never recommend a missing submission as its default. Keep omission
        # for the adversary so missed opposing submissions remain covered.
        return tuple(actions + ([OMIT] if player == 1 else []))

    def step(self, state, actions):
        """Ordinary simultaneous combat -> survivors move -> resource -> D loss.

        No events, invulnerability or area attacks are silently approximated.
        Dead fighters cannot respawn within a <=3-step initial-alive rollout.
        """
        nominal = [0.0, 0.0]
        earned = [0.0, 0.0]
        for i in range(2):
            j = 1 - i
            if state.hp[i] > 0 and state.hp[j] > 0 and actions[i][0] == "attack":
                if ray(state.positions[i], state.positions[j]) == actions[i][1]:
                    nominal[j] = 10.0 if actions[j] == DEFEND else 20.0
        hp = tuple(max(0.0, state.hp[i] - nominal[i]) for i in range(2))
        deaths = tuple(int(state.hp[i] > 0 and hp[i] <= 0) for i in range(2))
        for j in range(2):
            earned[1 - j] = min(state.hp[j], nominal[j]) + 60.0 * deaths[j]
        positions = list(state.positions)
        occupied = {positions[i] for i in range(2) if hp[i] > 0}
        wanted = {}
        for i in range(2):
            if hp[i] <= 0 or actions[i][0] != "move":
                continue
            dx, dy = DIRS[actions[i][1]]
            p = positions[i][0] + dx, positions[i][1] + dy
            if p in self.walkable and p not in occupied:
                wanted[i] = p
        for i, p in wanted.items():
            if list(wanted.values()).count(p) == 1:
                positions[i] = p
        resources = dict(state.resources)
        own_r = state.own_r
        for i in range(2):
            if hp[i] > 0 and positions[i] in resources:
                amount = resources.pop(positions[i])
                if i == 0:
                    own_r += amount
        return State(tuple(positions), hp, own_r,
                     max(0.0, state.own_d + earned[0] - 60.0 * deaths[0]),
                     tuple(sorted(resources.items())), state.own_deaths + deaths[0])

    def value(self, state):
        root = self.initial
        score = 30 * (min(state.own_r, 120) - min(root.own_r, 120)) / 120
        score += 15 * (min(state.own_d, self.damage_cap) - min(root.own_d, self.damage_cap)) / self.damage_cap
        result = score + self.weights["hp"] * (state.hp[0] - root.hp[0])
        result -= self.weights["death"] * state.own_deaths
        if self.goal is not None and state.hp[0] > 0:
            d0 = self.distances[root.positions[0]]
            distance = self.distances.get(state.positions[0], d0)
            result += self.weights["goal"] * (d0 - distance) / max(1, d0)
        return result

    def check_budget(self):
        if self.transitions >= self.joint_budget or time.perf_counter() >= self.deadline:
            raise BudgetExceeded

    def node(self, state, depth):
        self.check_budget()
        if depth == 0:
            value = self.value(state)
            return {"lower": value, "upper": value}
        key = state, depth
        if key in self.cache:
            return self.cache[key]
        rows, columns = self.choices(state, 0), self.choices(state, 1)
        lower, upper = [], []
        for a in rows:
            lo, hi = [], []
            for b in columns:
                self.check_budget()
                self.transitions += 1
                next_state = self.step(state, (a, b))
                if depth == 1:
                    v = self.value(next_state)
                    child = {"lower": v, "upper": v}
                else:
                    child = self.node(next_state, depth - 1)
                lo.append(child["lower"])
                hi.append(child["upper"])
            lower.append(lo)
            upper.append(hi)
        self.check_budget()
        sl = solve_matrix(lower, self.iterations, self.check_budget)
        su = sl if lower == upper else solve_matrix(upper, self.iterations, self.check_budget)
        self.check_budget()
        out = {"lower": sl.lower, "upper": su.upper, "policy": sl.row,
               "rows": rows, "columns": columns, "lower_matrix": lower, "upper_matrix": upper}
        self.cache[key] = out
        return out

    def search(self, sample_seed=0):
        started = time.perf_counter()
        self.deadline = started + self.time_ms / 1000.0
        self.transitions, self.cache = 0, {}
        completed, solution, exhausted = 0, None, False
        for depth in range(1, self.horizon + 1):
            try:
                candidate = self.node(self.initial, depth)
            except BudgetExceeded:
                exhausted = True
                break
            completed, solution = depth, candidate
        result = {"scope": "offline_closed_ordinary_duel_surrogate",
                  "requested_depth": self.horizon, "completed_depth": completed,
                  "budget_exhausted": exhausted, "joint_transitions": self.transitions,
                  "elapsed_ms": (time.perf_counter() - started) * 1000.0,
                  "score_damage_cap": self.damage_cap, "baseline_action": action_json(self.baseline),
                  "weights": self.weights, "sample_seed": sample_seed}
        if solution is None:
            return dict(result, recommendation="keep_baseline", policy=None,
                        sampled_action=action_json(self.baseline), certificate=None)
        index = solution["rows"].index(self.baseline)
        blo = min(solution["lower_matrix"][index])
        bhi = min(solution["upper_matrix"][index])
        # This compares both policies to the same adversarial continuation,
        # not to one hand-picked opponent or a partial row of a payoff matrix.
        override = solution["lower"] > bhi + self.weights["override_margin"]
        policy = [{"action": action_json(a), "probability": p}
                  for a, p in zip(solution["rows"], solution["policy"])]
        sample = random.Random(sample_seed).choices(solution["rows"], solution["policy"], k=1)[0]
        result.update(recommendation="candidate_exceeds_baseline_in_model" if override else "keep_baseline",
                      sampled_action=action_json(sample if override else self.baseline), policy=policy,
                      certificate={"lower": solution["lower"], "upper": solution["upper"],
                                   "gap": max(0.0, solution["upper"] - solution["lower"]),
                                   "baseline_lower": blo, "baseline_upper": bhi},
                      row_actions=[action_json(a) for a in solution["rows"]],
                      opponent_actions=[action_json(b) for b in solution["columns"]],
                      lower_matrix=solution["lower_matrix"], upper_matrix=solution["upper_matrix"])
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    parser.add_argument("--seed", type=int, default=0, help="reproducible local mixed-policy sample")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    raw = args.scene.read_bytes()
    try:
        scene = json.loads(raw)
        report = DuelLab(scene).search(args.seed)
    except (ValueError, KeyError, TypeError, IndexError) as exc:
        print(json.dumps({"error": str(exc), "status": "unsupported_or_invalid"}), file=sys.stderr)
        return 2
    report["scene_sha256"] = hashlib.sha256(raw).hexdigest()
    report["source_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report["scene_id"] = scene.get("id")
    encoded = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
