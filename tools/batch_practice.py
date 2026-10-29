#!/usr/bin/env python3
"""批量练习对局驱动器：不同地图/事件日程/出生点轮换，真实 deepseek-flash。
每局：start_referee.sh --config X --bots <原版陪练席位> (real) → 按 --our 逐个启动我方
席位二进制 → /api/resume → 等待 match completed → 提取摘要 → /api/stop + 收掉我方车 → 下一局。
产物：每局留在 ~/.rmtc-practice/practice-<id>/（供回放分析）；摘要汇总写 --summary。
自战（docs/38 联赛）：--our A:install --our B:install-v-old-B 表示 A/B 两席都跑我方二进制
（不同 install 底座），其余席位由裁判 spawn 原版陪练。席位二进制经 tools/run_seat.sh 启动，
节点名重映射 rmtc_bot_<seat> 避免双实例 ROS 命名冲突（源码与冠军二进制不变）。
密钥：只从环境 PRACTICE_LLM_KEY 或 ~/.rmtc-practice/llm.env 读取，绝不写进仓库。
"""
import argparse
import copy
import json
import os
import random
import re
import subprocess
import sys
import time
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
CST = os.path.dirname(TOOLS)
PORT = 8080
LLM_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-flash"
PROXY = "http://127.0.0.1:7890"

WIDTH, HEIGHT = 18, 18
SPAWNS = [(1, 1), (16, 1), (1, 16), (16, 16)]
CENTER = (9, 9)
EVENT_IDS = ["explosion", "illusion", "angel", "unrivaled", "world_split",
             "pepper", "purity", "pan"]


def llm_key():
    key = os.environ.get("PRACTICE_LLM_KEY", "").strip()
    if key:
        return key
    local = os.path.expanduser("~/.rmtc-practice/llm.env")
    if os.path.exists(local):
        for line in open(local):
            line = line.strip()
            if line.startswith("PRACTICE_LLM_KEY="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit("PRACTICE_LLM_KEY 未设置且 ~/.rmtc-practice/llm.env 不存在")


def env_for_referee():
    env = dict(os.environ)
    env.update({
        "PYTHONDONTWRITEBYTECODE": "1",
        "ROS_DOMAIN_ID": "27", "ROS_LOCALHOST_ONLY": "0",
        "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp",
        "PRACTICE_LLM_URL": LLM_URL, "PRACTICE_LLM_KEY": llm_key(),
        "PRACTICE_MODEL": MODEL,
        "http_proxy": PROXY, "https_proxy": PROXY, "all_proxy": PROXY,
        "HTTP_PROXY": PROXY, "HTTPS_PROXY": PROXY, "ALL_PROXY": PROXY,
        "NO_PROXY": "localhost,127.0.0.1,::1", "no_proxy": "localhost,127.0.0.1,::1",
    })
    return env


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def base_config():
    # 默认练习配置：取已归档对局的 config.json（即裁判默认配置）。
    src = "/home/frank/.rmtc-practice/practice-15465a23820b43d0b277d0f1de7fc655/config.json"
    with open(src) as f:
        return json.load(f)


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
    rot = vid % 4
    for i, r in enumerate(v["robots"]):
        r["spawn"] = list(SPAWNS[(i + rot) % 4])
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


def http_post(path):
    body = json.dumps({}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except Exception as e:  # noqa
        return 0, str(e)


def newest_run_dir(after):
    dirs = sorted(os.listdir("/home/frank/.rmtc-practice/"))
    for name in reversed(dirs):
        if name.startswith("practice-"):
            d = f"/home/frank/.rmtc-practice/{name}"
            if os.path.getmtime(d) >= after:
                return d
    return None


def stop_battle(procs):
    http_post("/api/stop")
    time.sleep(2)
    # 收掉我们的车（裁判/陪练由 practice.py 的 stop 清理）。
    for p in procs:
        if p.poll() is None:
            p.terminate()
    time.sleep(2)
    for p in procs:
        if p.poll() is None:
            p.kill()


def battle_summary(run_dir, variant_id):
    s = json.load(open(f"{run_dir}/score.json"))
    teams = {t["team"]: t for t in s["teams"]}
    ai = {}
    if os.path.exists(f"{run_dir}/ai.jsonl"):
        for line in open(f"{run_dir}/ai.jsonl"):
            r = json.loads(line)
            ai.setdefault(r["seat"], {"calls": 0, "fail": 0})
            ai[r["seat"]]["calls"] += 1
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
    log(f"battle {variant_id}: deploying config (our seats: {list(our_seats)})")
    before = time.time()
    referee_bots = ",".join(sorted(set("BCD") - set(our_seats)))
    ref = subprocess.Popen(
        ["bash", f"{CST}/practice/start_referee.sh", "--stage", "2",
         "--port", str(PORT), "--ai-mode", "real", "--bots", referee_bots,
         "--config", cfg_path],
        cwd=CST, env=env_for_referee(),
        stdout=open(f"{log_dir}/referee_stdout.log", "w"),
        stderr=subprocess.STDOUT, start_new_session=True)
    # 等待裁判就绪。
    deadline = time.time() + 60
    ready = False
    while time.time() < deadline:
        if ref.poll() is not None:
            return {"variant": variant_id, "error": "referee exited early"}
        try:
            out = open(f"{log_dir}/referee_stdout.log").read()
            if "练习裁判已就绪" in out:
                ready = True
                break
        except OSError:
            pass
        time.sleep(0.5)
    if not ready:
        stop_battle([ref])
        return {"variant": variant_id, "error": "referee not ready in 60s"}
    run_dir = newest_run_dir(before)
    if not run_dir:
        stop_battle([ref])
        return {"variant": variant_id, "error": "no run dir created"}
    # 启动我方各席位。
    bots = [spawn_our_seat(seat, install_base, log_dir)
            for seat, install_base in our_seats.items()]
    # 等 4 席注册（我方席位必须全在；陪练>=1）。
    deadline = time.time() + 90
    regs = []
    while time.time() < deadline:
        try:
            ref_log = open(f"{run_dir}/referee.log").read()
            regs = re.findall(r"registered team ([A-D])", ref_log)
        except OSError:
            regs = []
        if set(our_seats) <= set(regs) and len(set(regs)) >= 4:
            break
        time.sleep(0.5)
    if not set(our_seats) <= set(regs):
        stop_battle(bots + [ref])
        return {"variant": variant_id, "error": f"our seats not registered; got {regs}"}
    # 开赛。
    code, _ = http_post("/api/resume")
    if code != 200:
        stop_battle(bots + [ref])
        return {"variant": variant_id, "error": f"resume failed {code}"}
    # 等结束（500 tick × ~0.5s ≈ 250s + 余量）。
    deadline = time.time() + 600
    done = False
    while time.time() < deadline:
        try:
            if "match completed" in open(f"{run_dir}/referee.log").read():
                done = True
                break
        except OSError:
            pass
        time.sleep(2)
    if not done:
        stop_battle(bots + [ref])
        return {"variant": variant_id, "error": "match not completed in 600s"}
    summary = battle_summary(run_dir, variant_id)
    summary["ok"] = True
    log(f"battle {variant_id}: completed -> {summary['teams']}")
    stop_battle(bots + [ref])
    return summary


def parse_our(arg):
    seat, _, base = arg.partition(":")
    if seat not in "ABCD" or not base:
        raise SystemExit(f"--our 格式须为 SEAT:INSTALL_BASE（如 A:install），收到 {arg!r}")
    return seat, base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--seed", type=int, default=20261029)
    ap.add_argument("--summary", default="/tmp/batch_summary.json")
    ap.add_argument("--only", default=None,
                    help="逗号分隔的局号列表（如 0,1,2,3）；缺省全部")
    ap.add_argument("--our", action="append", default=None,
                    help="我方席位与 install 底座，可重复：SEAT:INSTALL_BASE；缺省 A:install")
    ap.add_argument("--map-locked", action="store_true",
                    help="同图同事件日程，只按 vid 轮换出生席位（docs/38 联赛按图成组）")
    args = ap.parse_args()
    our_seats = {}
    for a in (args.our or ["A:install"]):
        seat, base = parse_our(a)
        our_seats[seat] = base
    only = None
    if args.only is not None:
        only = {int(x) for x in args.only.split(",") if x.strip()}
    log_dir = f"/tmp/batch_practice_{args.seed}"
    os.makedirs(log_dir, exist_ok=True)
    rng = random.Random(args.seed)
    base = base_config()
    map_locked = make_map(base, rng) if args.map_locked else None
    results = []
    for vid in range(args.count):
        if only is not None and vid not in only:
            continue
        if args.map_locked:
            variant = apply_rotation(copy.deepcopy(map_locked), vid)
        else:
            variant = make_variant(base, rng, vid)
        ok, msg = validate(variant)
        if not ok:
            log(f"variant {vid}: REJECTED by validate_config: {msg}")
            results.append({"variant": vid, "error": "validate_config rejected"})
            continue
        cfg_path = f"{log_dir}/config_{vid}.json"
        with open(cfg_path, "w") as f:
            json.dump(variant, f, ensure_ascii=False, indent=1)
        try:
            results.append(run_battle(vid, variant, cfg_path, log_dir, our_seats))
        except Exception as e:  # noqa
            log(f"variant {vid}: exception {e}")
            results.append({"variant": vid, "error": str(e)})
            http_post("/api/stop")
        with open(args.summary, "w") as f:
            json.dump(results, f, ensure_ascii=False, indent=1)
        time.sleep(3)
    okn = sum(1 for r in results if r.get("ok"))
    log(f"batch done: {okn}/{len(results)} completed; summary -> {args.summary}")
    return 0 if okn else 1


if __name__ == "__main__":
    sys.exit(main())
