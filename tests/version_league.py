#!/usr/bin/env python3
"""冻结真实旧版本→生成分层局面→同一清单对打→逐局归档和成对报告；零模型费用。"""
import argparse
import copy
import fnmatch
import gzip
import json
import os
from pathlib import Path
import random
import selectors
import statistics
import subprocess
import sys
import time

import league_maps
import run_scenarios
import version_store

ROOT = run_scenarios.ROOT
SEATS = "ABCD"


class Lines:
    """有截止时间的JSONL子进程；失败直接留错，不把超时冒充某个合法动作。"""
    def __init__(self, command, error_path, timeout=5):
        self.error_file = Path(error_path).open("wb")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.error_file)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer, self.timeout = b"", timeout

    def call(self, request):
        self.process.stdin.write((json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode())
        self.process.stdin.flush()
        deadline = time.monotonic() + self.timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(remaining):
                raise TimeoutError("worker/engine response deadline exceeded")
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("worker/engine exited before complete response")
            self.buffer += chunk
            if len(self.buffer) > 16 * 1024 * 1024:
                raise ValueError("JSONL response exceeds 16MiB")
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()
        self.selector.close()
        self.error_file.close()


def condition(view, rule):
    """和短场景相同的JSON Pointer断点；路径拼错报错，不能静默假通过。"""
    if rule["path"] and not rule["path"].startswith("/"):
        raise ValueError("condition path must be a JSON Pointer")
    current, exists = view, True
    for token in rule["path"].split("/")[1:]:
        token = token.replace("~1", "/").replace("~0", "~")
        try:
            current = current[int(token)] if isinstance(current, list) else current[token]
        except (KeyError, ValueError, IndexError, TypeError):
            exists = False
            break
    op = rule["op"]
    if op == "exists":
        return exists
    if not exists:
        raise ValueError("condition path does not exist: " + rule["path"])
    value = rule["value"]
    if op == "eq": return current == value
    if op == "ne": return current != value
    if not isinstance(current, (float, int)) or isinstance(current, bool) or not isinstance(value, (float, int)) or isinstance(value, bool):
        raise ValueError("ordered condition requires numbers")
    operations = {"lt": lambda: current < value, "le": lambda: current <= value,
                  "gt": lambda: current > value, "ge": lambda: current >= value}
    if op not in operations:
        raise ValueError("unknown condition operator: " + op)
    return operations[op]()


def rank(result, focal):
    """正式比较键依次为分数、任务、R、D、较少无效动作；保留并列，不用HP破同分。"""
    def key(row):
        return tuple(row[k] for k in ("score", "qualified_M", "R", "D")) + (-row["invalid"],)
    own = key(result[SEATS.index(focal)])
    position = 1 + sum(key(row) > own for row in result)
    ties = sum(key(row) == own for row in result)
    return position, 1.0 / ties if position == 1 else 0.0


def play(case, label, plan, versions, engine_binary, engine_build, directory, timeout):
    """每场每个历史席位独立启动自己的worker，进程保留记忆；只有自己的input会出裁判。"""
    directory.mkdir(parents=True, exist_ok=True)
    scene, used = copy.deepcopy(case["scene"]), {}
    if any(key in scene["run"] for key in ("forks", "warm_until")):
        raise ValueError("league uses independent version processes; use tests/scripts/scenarios.sh for in-memory forks, or write separate paired cases")
    for seat, agent in scene["agents"].items():
        if agent["type"] == "external":
            name = agent["version"].replace("$focal", label).replace("$anchor", plan["anchor"])
            if name not in versions:
                raise ValueError("unknown frozen version: " + name)
            agent["version"] = name
            used[seat] = name
    signature = run_scenarios.digest(dict(case=case, candidate=label,
        versions={seat: versions[name][1]["binary_sha256"] for seat, name in used.items()}, engine=engine_build["build_id"]))
    existing = directory / "result.json"
    if existing.exists():
        result = json.loads(existing.read_text())
        if result["signature"] != signature:
            raise ValueError("case output belongs to different sources/fixture; choose another --out")
        if result["status"] == "passed":
            return result
    processes, workers = [], {}
    report = dict(case_id=case["id"], map_id=case["map_id"], family=case["family"], mix=case["mix"],
                  focal=case["focal"], candidate=label, signature=signature, scene=scene, versions=used,
                  engine_build_id=engine_build["build_id"], status="failed", scope="offline; no live model/ROS")
    state, previous = None, []
    durations, delivered, stalled = {seat: [] for seat in used}, {seat: set() for seat in used}, 0
    failures, stop = [], "until_tick"
    tape_path = directory / "tape.jsonl.gz"
    try:
        engine = Lines([str(engine_binary), "--serve"], directory / "engine.stderr.log", timeout)
        processes.append(engine)
        for seat, name in used.items():
            worker = Lines([str(versions[name][0])], directory / (seat + ".stderr.log"), timeout)
            processes.append(worker)
            hello = worker.call(dict(op="init", stage=scene["stage"]))
            if not hello["ready"] or hello["parameters"] != versions[name][1]["parameters"]:
                raise ValueError("worker differs from frozen parameter manifest")
            workers[seat] = worker
        state = engine.call(dict(scene=scene, build_id=engine_build["build_id"]))
        begin = state["world"]["tick"]
        until = scene["run"]["until_tick"]
        if not isinstance(until, int) or isinstance(until, bool) or not begin <= until <= scene["tick_limit"]:
            raise ValueError("until_tick outside fixture interval")
        with gzip.open(tape_path, "wt", encoding="utf-8", compresslevel=1) as tape:
            while True:
                if state["world"]["tick"] in scene["run"].get("save_at", []):
                    version_store.write_json(directory / ("tick-" + str(state["world"]["tick"]) + ".world.json"),
                                             dict(world=state["world"], note="world only; private worker memory is in live process, not serialized"))
                for rule in scene["run"].get("always", []):
                    if not condition(state, rule): failures.append(rule)
                if failures:
                    stop = "assertion_failed"
                    break
                if state["world"]["tick"] >= until:
                    break
                if any(condition(state, rule) for rule in scene["run"].get("break_when", [])):
                    stop = "breakpoint"
                    break
                answers = {}
                for seat, worker in workers.items():
                    message = state["inputs"][seat]
                    # 这是唯一发送给worker的对象：没有world、他队感知、未公告安排或未来资源表。
                    answer = worker.call(dict(op="act", input=message))
                    if answer["tick"] != state["world"]["tick"] or answer["session_id"] != scene["name"]:
                        raise ValueError("worker response tick/session mismatch")
                    durations[seat].append(answer["decide_ms"])
                    delivered[seat].update(event["key"] for event in message["events"])
                    answers[seat] = answer
                tape.write(json.dumps(dict(tick=state["world"]["tick"], inputs=state["inputs"], replies=answers), ensure_ascii=False) + "\n")
                tape.flush()
                state = engine.call(dict(op="step", actions={seat: answer["action"] for seat, answer in answers.items()}))
                car = state["world"]["cars"][case["focal"]]
                previous.append((tuple(car["position"]), car["R"], car["D"], tuple(car["completed"]), car["deaths"]))
                previous = previous[-12:]
                if len(previous) == 12 and car["alive"] and len({item[0] for item in previous}) <= 4 and len({item[1:] for item in previous}) == 1:
                    stalled += 1  # 候选徘徊窗口，不等于已证明卡住；防守也可能合理。
        for rule in scene["run"].get("expect", []):
            if not condition(state, rule): failures.append(rule)
        position, win = rank(state["result"], case["focal"])
        focal_result = state["result"][SEATS.index(case["focal"])]
        report.update(status="assertion_failed" if failures else "passed", stop=stop, failed_conditions=failures,
                      start_tick=begin, end_tick=state["world"]["tick"], result=state["result"],
                      focal_result=focal_result, rank=position, win_share=win, stationary_windows=stalled,
                      world=state["world"], timing={seat: dict(max_ms=max(values, default=0),
                        p95_ms=sorted(values)[max(0, (95 * len(values) + 99) // 100 - 1)] if values else 0,
                        over_500ms=sum(value > 500 for value in values)) for seat, values in durations.items()},
                      event_observations={seat: dict(unique_delivered=len(ids), delivered_ids=sorted(ids),
                        gained_fortress=state["world"]["cars"][seat]["empowered"],
                        gained_heal=state["world"]["cars"][seat]["heal_amount"] > 0,
                        causal_effect="not established by a single rollout") for seat, ids in delivered.items()})
    except (OSError, ValueError, KeyError, RuntimeError, TimeoutError) as error:
        report.update(error=str(error), last_state=state)
    finally:
        for process in reversed(processes): process.close()
    version_store.write_json(existing, report)
    if report["status"] != "passed" and state is not None:
        cold = copy.deepcopy(scene)
        cold["initial"] = dict(tick=state["world"]["tick"], resources=state["world"]["resources"],
                               cars={seat: state["world"]["cars"][seat] for seat in scene["agents"]})
        # 故障现场是可编辑冷局面；原策略记忆在tape中，不声称这是进程热快照。
        version_store.write_json(directory / "failure.cold.scene.json", cold)
    return report


def summarize(rows, plan):
    """比较同case的差值，按map_id聚合再自助采样；四席位不当作四张独立地图。"""
    summary = {}
    for label in plan["candidates"]:
        selected = [r for r in rows if r["candidate"] == label and r["status"] == "passed"]
        summary[label] = dict(completed=len(selected), failures=sum(r["candidate"] == label and r["status"] != "passed" for r in rows))
        if selected:
            summary[label].update(mean_score=statistics.mean(r["focal_result"]["score"] for r in selected),
                                  mean_rank=statistics.mean(r["rank"] for r in selected),
                                  win_share=statistics.mean(r["win_share"] for r in selected),
                                  mean_deaths=statistics.mean(r["focal_result"]["deaths"] for r in selected))
    paired, baseline = {}, plan["candidates"][0]
    valid = {(r["candidate"], r["case_id"]): r for r in rows if r["status"] == "passed"}
    for label in plan["candidates"][1:]:
        groups = {}
        for (name, case), row in valid.items():
            if name == label and (baseline, case) in valid:
                groups.setdefault(row["map_id"], []).append(row["focal_result"]["score"] - valid[baseline, case]["focal_result"]["score"])
        means = [statistics.mean(values) for values in groups.values()]
        if means:
            rng = random.Random(20261029)
            draws = sorted(statistics.mean(rng.choices(means, k=len(means))) for _ in range(1000))
            paired[label] = dict(reference=baseline, map_groups=len(means), paired_cases=sum(map(len, groups.values())),
                                 mean_map_delta=statistics.mean(means), bootstrap95=[draws[24], draws[974]],
                                 caution="descriptive; small fixture suite, not proof of official improvement")
    return dict(versions=summary, paired=paired)


def markdown(rows, summary, manifests, total_cases):
    """每局都列，不只挑胜局；错误与未跑独立列出，表头固定便于DeepSeek追加讨论。"""
    lines = ["# 冻结版本联赛", "", "范围：自建离线裁判；没有真实模型调用或原版DDS资格。", "",
             f"计划 {total_cases} 个版本×局面组合；本报告记录 {len(rows)} 个。重复运行可复用已通过的相同签名；未跑不算胜负。", "",
             "| 版本 | 源提交 | 二进制SHA256 |", "|---|---|---|"]
    for label, manifest in manifests.items():
        lines.append(f"| {label} | `{manifest['provenance']['commit'][:12]}` | `{manifest['binary_sha256'][:16]}…` |")
    lines += ["", "完整SHA、编译信息与全部参数见versions.json；表中仅缩短显示。"]
    lines += ["", "| 版本 | 完成 | 故障 | 均分 | 平均名次 | 第一份额 | 平均死亡 |", "|---|---:|---:|---:|---:|---:|---:|"]
    for label, item in summary["versions"].items():
        lines.append(f"| {label} | {item['completed']} | {item['failures']} | " + " | ".join(f"{item[k]:.3f}" if k in item else "—" for k in ("mean_score", "mean_rank", "win_share", "mean_deaths")) + " |")
    lines += ["", "| 候选相对首版本 | 地图组 | 配对局面 | 地图均分差 | 分组自助95%区间 |", "|---|---:|---:|---:|---|"]
    for label, item in summary["paired"].items():
        lines.append(f"| {label} | {item['map_groups']} | {item['paired_cases']} | {item['mean_map_delta']:.3f} | {item['bootstrap95']} |")
    lines += ["", "区间仅描述本夹具样本；同图席位/对手的相关性按地图聚合，少量地图不能证明泛化。"]
    last_family = None
    for row in rows:
        if row["family"] != last_family:
            last_family = row["family"]
            lines += ["", "## " + last_family, "",
                      "| 局面 | 版本 | 状态 | Tick止点 | 名次 | 分 | R | D | 任务 | 死亡 | 无效 | 徘徊窗口* |",
                      "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
        result = row.get("focal_result", {})
        values = [row["case_id"], row["candidate"], row["status"], row.get("end_tick", "—"), row.get("rank", "—"),
                  *[result.get(k, "—") for k in ("score", "R", "D", "qualified_M", "deaths", "invalid")], row.get("stationary_windows", "—")]
        lines.append("| " + " | ".join(map(str, values)) + " |")
    lines += ["", "*徘徊窗口：连续12帧在≤4格、R/D/任务/死亡无进展的候选标签；合理防守也会触发，须看tape。", "",
              "每局目录有result.json、tape.jsonl.gz和stderr；失败另存failure.cold.scene.json。完整参数在versions.json。",
              "公告送达/授予只算观察记录；因果收益需同资格、同局面、同对手的有/无解释或动作机制配对。", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    freezing = commands.add_parser("freeze", help="从真实Git提交冻结历史worker")
    freezing.add_argument("label")
    freezing.add_argument("--ref", help="commit/tag/branch；解析后固定完整SHA")
    freezing.add_argument("--repository", default=str(ROOT))
    freezing.add_argument("--snapshot", help="替代git读取的已取得源码目录；必须同时给--lock")
    freezing.add_argument("--lock", help="含repository/commit/git_blobs的快照清单")
    freezing.add_argument("--store", default=str(ROOT / "build/league/versions"))
    freezing.add_argument("--json-include")
    suite = commands.add_parser("suite", help="写12局面×4席位×3组合的可编辑清单")
    suite.add_argument("--versions", nargs="+", required=True, help="label=冻结目录")
    suite.add_argument("--anchor", required=True)
    suite.add_argument("--seed", type=int, default=20261029)
    suite.add_argument("--out", required=True)
    running = commands.add_parser("run", help="执行清单，可按case过滤/限制；同签名已通过项可续跑")
    running.add_argument("plan")
    running.add_argument("--out", required=True)
    running.add_argument("--cases", default="*", help="case id通配符，例如bridge-*-pressure")
    running.add_argument("--limit", type=int, help="最多多少个case；每个case运行全部候选")
    running.add_argument("--json-include")
    running.add_argument("--worker-timeout", type=float, default=5, help="进程硬时限秒；另报策略超过500ms数")
    args = parser.parse_args()
    if args.command == "freeze":
        if bool(args.snapshot) != bool(args.lock) or bool(args.ref) == bool(args.snapshot):
            parser.error("freeze二选一：--ref 或 --snapshot 加 --lock")
        sources, provenance = version_store.locked_sources(args.snapshot, args.lock) if args.snapshot else version_store.git_sources(args.repository, args.ref)
        folder, manifest = version_store.freeze(args.label, sources, provenance, args.store, args.json_include)
        print(json.dumps(dict(version=str(folder), commit=manifest["provenance"]["commit"], binary_sha256=manifest["binary_sha256"])))
        return 0
    if args.command == "suite":
        versions = dict(entry.split("=", 1) for entry in args.versions)
        if args.anchor not in versions or len(versions) < 2:
            raise ValueError("suite needs two frozen labels and an existing anchor")
        versions = {label: str(Path(path).resolve()) for label, path in versions.items()}
        plan = league_maps.make_suite(list(versions), args.anchor, versions, args.seed)
        version_store.write_json(args.out, plan)
        print(json.dumps(dict(plan=args.out, cases=len(plan["cases"]), combinations=len(plan["cases"]) * len(versions))))
        return 0
    if args.worker_timeout <= 0 or args.limit is not None and args.limit <= 0:
        raise ValueError("positive timeout/limit required")
    plan = json.loads(Path(args.plan).read_text())
    if plan["schema"] != 1 or len(set(c["id"] for c in plan["cases"])) != len(plan["cases"]):
        raise ValueError("plan schema/duplicate case id")
    versions = {label: version_store.load_version(folder) for label, folder in plan["versions"].items()}
    engine_binary, engine_build = run_scenarios.build(args.json_include)
    destination = Path(args.out).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    manifests = {label: entry[1] for label, entry in versions.items()}
    identity = dict(plan_sha256=run_scenarios.digest(plan), engine_build_id=engine_build["build_id"],
                    binaries={label: manifest["binary_sha256"] for label, manifest in manifests.items()})
    receipt = destination / "identity.json"
    if receipt.exists() and json.loads(receipt.read_text()) != identity:
        raise ValueError("output directory belongs to a different plan/build; choose another --out")
    version_store.write_json(receipt, identity)
    version_store.write_json(destination / "versions.json", manifests)
    version_store.write_json(destination / "plan.json", plan)
    version_store.write_json(destination / "engine.json", engine_build)
    cases = [case for case in plan["cases"] if fnmatch.fnmatchcase(case["id"], args.cases)]
    if args.limit: cases = cases[:args.limit]
    if not cases: raise ValueError("no selected cases")
    rows = []
    for case in cases:
        if not all(part and part.replace("-", "").replace("_", "").isalnum() for part in [case["id"], *plan["candidates"]]):
            raise ValueError("unsafe case/version filename")
        for label in plan["candidates"]:
            row = play(case, label, plan, versions, engine_binary, engine_build, destination / case["id"] / label, args.worker_timeout)
            rows.append(row)
            summary = summarize(rows, plan)
            version_store.write_json(destination / "summary.json", dict(summary=summary, rows=rows))
            (destination / "report.md").write_text(markdown(rows, summary, manifests, len(plan["cases"]) * len(plan["candidates"])))
            print(json.dumps(dict(case=case["id"], version=label, status=row["status"], score=row.get("focal_result", {}).get("score")), ensure_ascii=False), flush=True)
    return 1 if any(row["status"] != "passed" for row in rows) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("版本联赛失败：" + str(error), file=sys.stderr)
        sys.exit(2)
