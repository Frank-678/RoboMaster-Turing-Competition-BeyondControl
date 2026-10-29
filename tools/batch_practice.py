#!/usr/bin/env python3
"""批量练习对局驱动器：不同地图/事件日程/出生点轮换，真实 deepseek-flash。
每局：start_referee.sh --config X --bots <原版陪练席位> (real) → 按 --our 逐个启动我方
席位二进制 → /api/resume → 等待 match completed → 提取摘要 → /api/stop + 收掉我方车 → 下一局。
产物：每局留在 ~/.rmtc-practice/practice-<id>/（供回放分析）；摘要汇总写 --summary。
自战（docs/38 联赛）：--our A:install --our B:install-v-old-B 表示 A/B 两席都跑我方二进制
（不同 install 底座），其余席位由裁判 spawn 原版陪练。席位二进制经 tools/run_seat.sh 启动，
节点名重映射 rmtc_bot_<seat> 避免双实例 ROS 命名冲突（源码与冠军二进制不变）。
网络配置：只经 bash practice_batch.sh 从已编译 match_config.hpp 交接；不另设模型/密钥文件。
"""
import argparse
import copy
import hashlib
from pathlib import Path
import tempfile
import json
import os
import random
import re
import signal
import subprocess
import sys
import time
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
CST = os.path.dirname(TOOLS)
REFEREE_STARTUP_RETRIES = 3   # 原版 referee 12s 节点就绪竞态（practice.py:279）：背靠背开局偶发 → 静置重试
REFEREE_STARTUP_SETTLE = 15   # 重试前静置秒数：让 ROS/DDS 与上一局残留节点完全退出
INTER_BATTLE_SETTLE = 3      # 局间静置：stop_battle 后 ROS/DDS 异步拆除完成再开下一局


def referee_ready_since(log_dir, baseline):
    """按尝试独立的就绪判定：baseline 为 (就绪行数, 文件 mtime_ns)，每次尝试
    启动前采集。就绪当且仅当自那以后文件新增了就绪行——追加增长（行数超过
    基线）或截断重开（mtime 前进、行数回落/持平）时以当前已出现就绪行为准。
    防上一尝试残留行误认，也防截断后同计数新行漏认（review_handoff_probe R2）。"""
    count_before, mtime_before = baseline
    path = Path(log_dir, "referee_stdout.log")
    try:
        text = path.read_text()
        mtime = path.stat().st_mtime_ns
    except FileNotFoundError:
        return False
    count = text.count("练习裁判已就绪")
    if count > count_before:
        return True
    return mtime > mtime_before and count >= 1

WIDTH, HEIGHT = 18, 18
SPAWNS = [(1, 1), (16, 1), (1, 16), (16, 16)]
CENTER = (9, 9)
EVENT_IDS = ["explosion", "illusion", "angel", "unrivaled", "world_split",
             "pepper", "purity", "pan"]


def env_for_referee():
    """启动器已经覆盖网络/模型配置；不在Python里猜值或读第二份密钥。"""
    required = ("PRACTICE_LLM_URL", "PRACTICE_LLM_KEY", "PRACTICE_MODEL",
                "ROS_DOMAIN_ID", "ROS_LOCALHOST_ONLY", "RMW_IMPLEMENTATION",
                "RMTC_PRACTICE_PORT")
    if any(not os.environ.get(k) for k in required):
        raise SystemExit("请用 bash practice_batch.sh；网络只配置match_config.hpp并重新构建。")
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def config_digest(cfg):
    """语义相同的JSON得到相同摘要，忽略缩进与键顺序。"""
    return hashlib.sha256(json.dumps(cfg, sort_keys=True, separators=(",", ":")).encode()).hexdigest()



def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def base_config(path=None):
    # 只读取原版模板；不依赖某人的历史对局目录，也不写practice/。
    with open(path or f"{CST}/practice/config/stage2.json") as stream:
        return json.load(stream)


def free_cells(cfg):
    blocked = set(SPAWNS) | {CENTER}
    blocked |= {tuple(o) for o in cfg["map"]["obstacles"]}
    cells = [(x, y) for x in range(0, WIDTH) for y in range(0, HEIGHT)
             if (x, y) not in blocked and not (x == CENTER[0] and y == CENTER[1])]
    return cells


def make_map(cfg, rng):
    """生成一张图：障碍/资源/事件日程/事件参数扰动（不含席位轮换，不消耗额外 rng）。"""
    v = copy.deepcopy(cfg)
    cells = free_cells(v)
    # 1) 障碍：6 堵墙随机重摆（不堵出生点/中心/彼此）。
    rng.shuffle(cells)
    walls = []
    used = set(SPAWNS) | {CENTER}
    for (x, y) in cells:
        if len(walls) == 6:
            break
        if (x, y) in used:
            continue
        # 避免把出生点的两个直邻都堵死（留一个出口）。
        near_spawn = any(abs(x - sx) + abs(y - sy) == 1 for sx, sy in SPAWNS)
        if near_spawn and len(walls) % 2 == 0:
            continue
        walls.append([x, y])
        used.add((x, y))
    v["map"]["obstacles"] = walls
    # 2) 资源：16 份重摆到剩余空格，价值 12×10 + 4×20。
    cells = free_cells(v)
    rng.shuffle(cells)
    res = []
    values = [10] * 12 + [20] * 4
    rng.shuffle(values)
    for i, (x, y) in enumerate(cells[:16]):
        res.append({"id": f"practice_{i}", "position": [x, y], "value": values[i]})
    v["map"]["resources"] = res
    # 3) 事件日程：8 事件打乱顺序，公告 tick 在 10..220 间铺开（保留间隔>=15）。
    order = EVENT_IDS[:]
    rng.shuffle(order)
    ticks = sorted(rng.sample(range(10, 221, 5), 8))
    v["events"]["schedule"] = [
        {"id": f"practice_{e}", "event": e, "announce_tick": t}
        for e, t in zip(order, ticks)]
    # 4) 事件参数扰动（保留官方模板结构；validate_config 不通过则由 gate 丢弃）。
    d = v["events"]["definitions"]
    if "explosion" in d:
        r = d["explosion"]["rect"]
        dx, dy = rng.randint(1, 6), rng.randint(1, 6)
        r["top_left"] = [dx, dy]
        r["bottom_right"] = [min(17, dx + 4), min(17, dy + 4)]
        d["explosion"]["damage"] = rng.choice([20, 35, 50])
    if "pepper" in d:
        pts = []
        for _ in range(2):
            pts.append([rng.randint(1, 16), rng.randint(1, 16)])
        d["pepper"]["points"] = pts
    if "angel" in d:
        pts = []
        for _ in range(2):
            pts.append([rng.randint(1, 16), rng.randint(1, 16)])
        d["angel"]["points"] = pts
    return v


def apply_rotation(v, vid):
    """出生点席位轮换（纯 vid 函数，不消耗 rng）。"""
    seats = ["A", "B", "C", "D"]
    if sorted(r["team"] for r in v["robots"]) != seats:
        raise ValueError("批量对抗需要四个不重复席位A/B/C/D")
    rot = vid % 4
    for r in v["robots"]:
        r["spawn"] = list(SPAWNS[(seats.index(r["team"]) + rot) % 4])
    return v


def make_variant(cfg, rng, vid):
    return apply_rotation(make_map(cfg, rng), vid)


def validate(cfg):
    env = dict(os.environ)
    env.update({"PYTHONDONTWRITEBYTECODE": "1"})
    try:
        p = subprocess.run([f"{CST}/practice/bin/validate_config"],
                           input=json.dumps(cfg), text=True,
                           capture_output=True, timeout=20, env=env)
        return p.returncode == 0, (p.stdout + p.stderr)[:400]
    except Exception as e:  # noqa
        return False, f"validate crashed: {e}"


def http_request(path, body=None):
    """本机控制口必须直连；不把127.0.0.1请求交给出网代理。"""
    port = int(os.environ["RMTC_PRACTICE_PORT"])
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                                 headers={"Content-Type": "application/json"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(req, timeout=10) as response:
            return response.status, json.loads(response.read())
    except (OSError, ValueError):
        return 0, {}


def http_post(path):
    return http_request(path, {})


def owned_run(state, cfg, after):
    """通过当前服务的session和完整配置确认目录归属，不能挑uuid排序最大的目录。"""
    run = Path(state.get("run_dir", ""))
    session = state.get("session_id", "")
    if not re.fullmatch(r"practice-[0-9a-f]{32}", session) or run.name != session:
        raise ValueError("裁判未给出可核对的session/run_dir")
    if state.get("settings", {}).get("config") != cfg or run.stat().st_mtime < after - 1:
        raise ValueError("裁判场次或配置不属于本次启动")
    if json.loads((run / "config.json").read_text()) != cfg:
        raise ValueError("场次归档配置与请求不一致")
    return run


def stop_battle(procs, session=None):
    # 只停止刚确认归属的场次，避免端口被别的练习占用时误停他人的比赛。
    if session:
        _, state = http_request("/api/state")
        if state.get("session_id") == session:
            http_post("/api/stop")
    # 组长已退出不代表组内子进程都退出；每个记录过的独立进程组都要收。
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for process in procs:
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
        for process in procs:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass


def battle_summary(run_dir, variant_id):
    s = json.load(open(f"{run_dir}/score.json"))
    teams = {t["team"]: t for t in s["teams"]}
    ai = {}
    if os.path.exists(f"{run_dir}/ai.jsonl"):
        for line in open(f"{run_dir}/ai.jsonl"):
            r = json.loads(line)
            ai.setdefault(r["seat"], {"calls": 0, "success": 0, "fail": 0})
            ai[r["seat"]]["calls"] += 1
            ai[r["seat"]]["success"] += int(bool(r.get("success")))
            if not r.get("success"):
                ai[r["seat"]]["fail"] += 1
    return {
        "variant": variant_id, "session": os.path.basename(run_dir),
        "teams": {k: {"score": v["score"], "R": v["resource_value"],
                      "D": v["effective_damage"], "M": v["mission_completed"],
                      "hp": v["hp"], "invalid": v["invalid_actions"]}
                  for k, v in teams.items()},
        "ai": ai,
    }


def spawn_our_seat(seat, install_base, log_dir):
    """按席位启动我方二进制：tools/run_seat.sh <install_base> <seat> <log>（脚本自行写日志）"""
    run_script = f"{TOOLS}/run_seat.sh"
    log_file = f"{log_dir}/bot{seat}.log"
    return subprocess.Popen(["bash", run_script, install_base, seat, log_file],
                            cwd=CST, start_new_session=True)


def run_battle(variant_id, cfg, cfg_path, log_dir, our_seats):
    """每局单独日志，所有异常都走finally回收；失败也保留场次和输入。"""
    log(f"battle {variant_id}: deploying (our seats: {list(our_seats)})")
    before = time.time()
    result = {"variant": variant_id, "ok": False, "log_dir": str(Path(log_dir).resolve()),
              "config_path": str(Path(cfg_path).resolve()), "config_sha256": config_digest(cfg)}
    processes, session = [], None
    stream = open(f"{log_dir}/referee_stdout.log", "w")
    try:
        referee_bots = ",".join(sorted(set("ABCD") - set(our_seats)))
        ref = None
        for attempt in range(1, REFEREE_STARTUP_RETRIES + 1):
            try:
                # 每次尝试独立就绪基线（行数 + mtime）：防止上一尝试残留的 Ready 行
                # 误判本次已就绪，也防止裁判日志截断重开后同计数新行漏认。
                _ref_path = Path(log_dir, "referee_stdout.log")
                try:
                    _ref_stat = _ref_path.stat()
                    ready_baseline = (_ref_path.read_text().count("练习裁判已就绪"),
                                      _ref_stat.st_mtime_ns)
                except FileNotFoundError:
                    ready_baseline = (0, 0)
                ref = subprocess.Popen(
                    ["bash", f"{CST}/practice/start_referee.sh", "--stage", "2",
                     "--port", os.environ["RMTC_PRACTICE_PORT"], "--ai-mode", "real", "--bots", referee_bots,
                     "--config", cfg_path], cwd=CST, env=env_for_referee(), stdout=stream,
                    stderr=subprocess.STDOUT, start_new_session=True)
                processes.append(ref)
                deadline = time.monotonic() + 60
                while True:
                    if ref.poll() is not None:
                        raise RuntimeError("referee exited before ready")
                    if referee_ready_since(log_dir, ready_baseline):
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError("referee not ready in 60s")
                    time.sleep(0.5)
                break
            except RuntimeError as error:
                last_error = error
                log(f"battle {variant_id}: referee 启动失败（第 {attempt}/{REFEREE_STARTUP_RETRIES} 次）：{error}；"
                    f"静置 {REFEREE_STARTUP_SETTLE}s 后重试")
                if ref is not None and ref.poll() is None:
                    try:
                        os.killpg(ref.pid, signal.SIGTERM)  # start_new_session：整组收掉残留 ROS 节点
                    except ProcessLookupError:
                        pass
                    try:
                        ref.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(ref.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                processes = [p for p in processes if p is not ref]
                stream.write(f"\n[driver] referee 启动重试：第 {attempt} 次失败，静置后第 {attempt + 1} 次\n")
                stream.flush()
                time.sleep(REFEREE_STARTUP_SETTLE)
                ref = None
        else:
            raise RuntimeError(f"referee 连续 {REFEREE_STARTUP_RETRIES} 次启动失败：{last_error}")
        code, state = http_request("/api/state")
        if code != 200:
            raise RuntimeError("cannot read referee state")
        run_dir = owned_run(state, cfg, before)
        session = state["session_id"]
        result.update(session=session, run_dir=str(run_dir))
        for seat, install_base in our_seats.items():
            processes.append(spawn_our_seat(seat, install_base, log_dir))
        deadline = time.monotonic() + 90
        while True:
            ref_log = (run_dir / "referee.log").read_text() if (run_dir / "referee.log").exists() else ""
            regs = set(re.findall(r"registered team ([A-D])", ref_log))
            if regs == set("ABCD"):
                break
            if any(p.poll() is not None for p in processes):
                raise RuntimeError("a contestant or referee exited before all registrations")
            if time.monotonic() >= deadline:
                raise RuntimeError(f"four seats not registered; got {sorted(regs)}")
            time.sleep(0.5)
        if http_post("/api/resume")[0] != 200:
            raise RuntimeError("resume rejected")
        deadline = time.monotonic() + 600
        while "match completed" not in (run_dir / "referee.log").read_text():
            if any(p.poll() is not None for p in processes):
                raise RuntimeError("a contestant or referee exited during battle")
            if time.monotonic() >= deadline:
                raise RuntimeError("match not completed in 600s")
            time.sleep(2)
        result.update(battle_summary(str(run_dir), variant_id), ok=True)
        return result
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        return result
    finally:
        stop_battle(processes, session)
        stream.close()
        time.sleep(INTER_BATTLE_SETTLE)


def parse_our(arg):
    seat, _, base = arg.partition(":")
    if seat not in set("ABCD") or not base:
        raise SystemExit(f"--our 格式须为 SEAT:INSTALL_BASE（如 A:install），收到 {arg!r}")
    return seat, base


def variants(cfg, seed, count, map_locked=False):
    """先生成所有编号再筛选；--only不会改变被补跑编号的随机流。"""
    rng = random.Random(seed)
    fixed = make_map(cfg, rng) if map_locked else None
    for vid in range(count):
        yield vid, apply_rotation(copy.deepcopy(fixed), vid) if map_locked else make_variant(cfg, rng, vid)


def seat_hashes(our_seats):
    """启动前记录实际执行文件身份，日志中的版本名不能代替程序摘要。"""
    result = {}
    for seat, base in our_seats.items():
        path = Path(base) if os.path.isabs(base) else Path(CST, base)
        binary = path / "rmtc_bot/lib/rmtc_bot/bot"
        result[seat] = {"install_base": str(path.resolve()),
                        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--seed", type=int, default=20261029)
    ap.add_argument("--summary", default="/tmp/batch_summary.json")
    ap.add_argument("--base-config", help="可替换练习模板，只读；默认practice/config/stage2.json")
    ap.add_argument("--only", help="逗号分隔的局号；补跑与完整批次同编号保持同地图")
    ap.add_argument("--our", action="append", help="SEAT:INSTALL_BASE；缺省A:install")
    ap.add_argument("--map-locked", action="store_true", help="同图同事件，仅轮换四席出生点")
    args = ap.parse_args()
    env_for_referee()  # 开始前一次检查配置，不在跑到一半时才发现缺模型/密钥。
    if args.count < 1:
        ap.error("count必须为正")
    our_seats = {}
    for arg in args.our or ["A:install"]:
        seat, base = parse_our(arg)
        if seat in our_seats:
            ap.error("--our不能重复席位")
        our_seats[seat] = base
    only = set(range(args.count)) if args.only is None else {int(x) for x in args.only.split(",") if x.strip()}
    if not only or not only <= set(range(args.count)):
        ap.error("--only编号超出本批count或为空")
    identities = seat_hashes(our_seats)
    summary_path = Path(args.summary).resolve()
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    log_root = summary_path.parent / (summary_path.stem + ".runs")
    log_root.mkdir(exist_ok=True)
    prior = json.loads(summary_path.read_text()) if summary_path.exists() else []
    results = {e["variant"]: e for e in prior}
    for vid, variant in variants(base_config(args.base_config), args.seed, args.count, args.map_locked):
        if vid not in only:
            continue
        digest = config_digest(variant)
        previous = results.get(vid)
        if previous and (previous.get("config_sha256") != digest or previous.get("binaries") != identities):
            raise SystemExit("已有摘要的地图/程序身份不同，请为本批指定新的--summary；未覆盖旧产物。")
        if previous and previous.get("ok"):
            log(f"variant {vid}: cached complete with same map/binaries")
            continue
        log_dir = Path(tempfile.mkdtemp(prefix=f"case-{vid}-", dir=log_root))
        cfg_path = log_dir / f"config_{vid}.json"
        cfg_path.write_text(json.dumps(variant, ensure_ascii=False, indent=1))
        ok, message = validate(variant)
        if ok:
            entry = run_battle(vid, variant, str(cfg_path), str(log_dir), our_seats)
        else:
            entry = {"variant": vid, "ok": False, "error": "validate_config rejected: " + message}
        entry.update(config_sha256=digest, binaries=identities, log_dir=str(log_dir), config_path=str(cfg_path))
        results[vid] = entry
        # 包括最后一个被校验拒绝的场景；失败不能在摘要中消失。
        temporary = summary_path.with_suffix(summary_path.suffix + ".tmp")
        temporary.write_text(json.dumps([results[k] for k in sorted(results)], ensure_ascii=False, indent=1))
        temporary.replace(summary_path)
    okn = sum(bool(results.get(vid, {}).get("ok")) for vid in only)
    log(f"batch done: {okn}/{len(only)} requested completed; summary -> {summary_path}")
    return 0 if okn == len(only) else 1


if __name__ == "__main__":
    sys.exit(main())
