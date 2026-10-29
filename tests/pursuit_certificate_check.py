#!/usr/bin/env python3
"""Optional independent LP audit; SciPy is a checker, not a runtime dependency."""
import argparse
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pursuit_lab import DuelLab, solve_matrix
from eval_pursuit_lab import fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        import numpy as np
        from scipy.optimize import linprog
        import scipy
    except ImportError:
        parser.error("optional checker needs scipy and numpy; planner does not")

    def lp(matrix):
        a = np.array(matrix, dtype=float)
        m, n = a.shape
        cost = np.zeros(m + 1)
        cost[-1] = -1
        result = linprog(cost, A_ub=np.column_stack((-a.T, np.ones(n))), b_ub=np.zeros(n),
                         A_eq=[[*np.ones(m), 0]], b_eq=[1],
                         bounds=[(0, None)] * m + [(None, None)], method="highs")
        if not result.success:
            raise RuntimeError(result.message)
        return float(result.x[-1])

    rng, matrix_rows, tree_rows = random.Random(20261029), [], []
    for index in range(80):
        matrix = [[rng.uniform(-10, 10) for _ in range(rng.randint(2, 8))] for _ in range(1)]
        matrix = matrix + [[rng.uniform(-10, 10) for _ in matrix[0]] for _ in range(rng.randint(1, 7))]
        solution, exact = solve_matrix(matrix, 1024), lp(matrix)
        matrix_rows.append({"id": index, "matrix": matrix, "lp_value": exact,
                            "lower": solution.lower, "upper": solution.upper,
                            "passed": solution.lower <= exact + 1e-8 and exact <= solution.upper + 1e-8})
    for fixture in fixtures():
        for cap in (300, 500):
            raw = dict(fixture, score_damage_cap=cap)
            raw["search"] = dict(raw["search"], time_budget_ms=10000)
            lab = DuelLab(raw)
            @lru_cache(None)
            def exact_value(state, depth):
                if depth == 0:
                    return lab.value(state)
                return lp([[exact_value(lab.step(state, (a, b)), depth - 1)
                            for b in lab.choices(state, 1)] for a in lab.choices(state, 0)])
            report = lab.search(20261029)
            exact = exact_value(lab.initial, lab.horizon)
            cert = report["certificate"]
            matrix = [[exact_value(lab.step(lab.initial, (a, b)), lab.horizon - 1)
                       for b in lab.choices(lab.initial, 1)] for a in lab.choices(lab.initial, 0)]
            x = [p["probability"] for p in report["policy"]]
            true_policy_security = min(sum(x[i] * matrix[i][j] for i in range(len(x)))
                                       for j in range(len(matrix[0])))
            good = (report["completed_depth"] == lab.horizon and cert["lower"] <= exact + 1e-8
                    and exact <= cert["upper"] + 1e-8 and cert["lower"] <= true_policy_security + 1e-8)
            tree_rows.append({"scene_id": f"{fixture['id']}-cap{cap}", "lp_value": exact,
                              "lower": cert["lower"], "upper": cert["upper"],
                              "policy_security_by_lp_children": true_policy_security,
                              "states_checked": exact_value.cache_info().currsize, "passed": good})
    result = {"schema": 1, "scope": "independent HiGHS LP check of finite surrogate value bounds",
              "scipy_version": scipy.__version__,
              "planner_sha256": hashlib.sha256((Path(__file__).resolve().parents[1] / "tools/pursuit_lab.py").read_bytes()).hexdigest(),
              "matrix_planned": 80, "matrix_passed": sum(x["passed"] for x in matrix_rows),
              "tree_planned": 20, "tree_passed": sum(x["passed"] for x in tree_rows),
              "matrix_rows": matrix_rows, "tree_rows": tree_rows}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("matrix_planned", "matrix_passed", "tree_planned", "tree_passed")}))
    return 0 if result["matrix_passed"] == 80 and result["tree_passed"] == 20 else 1


if __name__ == "__main__":
    raise SystemExit(main())
