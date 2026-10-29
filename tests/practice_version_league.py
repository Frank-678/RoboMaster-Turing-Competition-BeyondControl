#!/usr/bin/env python3
"""版本联赛调度器（docs/38 §2）：12 图 × 4 席位 × 4 混合 = 192 场/版本。
每个 (图 seed, 混合) 跑一次驱动（--map-locked --count 4）：同图同事件日程，
4 局仅轮换出生席位，消出生位置偏差。混合定义：
  M1 原版：A=本版本（champion），B/C/D=原版陪练（裁判 spawn）。
  M2 自战·旧冠军：A=champion，B=old_champion（冻结席位变体），C/D=原版陪练。
  M3 自战·候选（对手位）：A=champion，B=candidate，C/D=原版陪练。
  M4 自战·候选（焦点位，docs/48 依 ChatGPT CG-C-20261029-HANDOFF-FINAL 第 1 条增）：
     A=candidate，B=champion（席位变体），C/D=原版陪练。与 M1 同地图/同 A 席位，
     但 B 位为 champion（非原版陪练）——对手池与 M1 不同，M3/M4 构成冠军×候选换位自战对。
  M5 原版·候选（同池焦点对照，review_handoff_probe R1 后补）：A=candidate，B/C/D=原版陪练，
     与 M1 完全同对手池；用于候选与冠军在同图同席位同池下的直接成对比较。
冻结契约（U6 条款）：开赛前逐一校验参战二进制 sha256 与清单一致，不符即中止。
用法：
  bash practice_league.sh --maps 20261201,20261202 --mixes M1,M2 [--out DIR]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

CST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MIXES = {
    "M1": {"A": "champion"},
    "M2": {"A": "champion", "B": "old_champion"},
    "M3": {"A": "champion", "B": "candidate"},
    "M4": {"A": "candidate", "B": "champion"},
    "M5": {"A": "candidate"},
}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def resolve_base(base):
    """install 底座：绝对路径原样；相对路径相对代码仓根（驱动 cwd）。"""
    return base if os.path.isabs(base) else f"{CST}/{base}"


def seat_base(manifest, version, seat):
    """(版本, 席位) 的 install 底座：champion-A 用冠军底座；其余用席位变体。"""
    if version == "champion" and seat == "A":
        return manifest["versions"]["champion"]["install_base"]
    return manifest["seats"][version][seat]["install_base"]


def verify_frozen(manifest):
    """冻结契约：每个参战二进制 sha256 必须等于清单记录值。"""
    checks = []
    v = manifest["versions"]["champion"]
    checks.append(("champion install bot",
                   f"{resolve_base(v['install_base'])}/rmtc_bot/lib/rmtc_bot/bot",
                   v["binary_sha256"]))
    for version, seats in (manifest.get("seats") or {}).items():
        for seat, rec in seats.items():
            checks.append((f"{version}-{seat} bot",
                           f"{resolve_base(rec['install_base'])}/rmtc_bot/lib/rmtc_bot/bot",
                           rec["binary_sha256"]))
    for label, path, want in checks:
        if not os.path.isfile(path):
            raise SystemExit(f"冻结校验失败：{label} 不存在：{path}")
        got = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if got != want:
            raise SystemExit(f"冻结校验失败：{label} sha256 {got[:12]}… != 清单 {want[:12]}…")
        log(f"冻结校验通过：{label}")


def run_mix(seed, mix, our, out_dir, args):
    """对一个 (图, 混合) 跑驱动 --map-locked --count 4，返回 (ok_battles, total)。"""
    summary = os.path.join(out_dir, f"summary_{mix}_{seed}.json")
    cmd = ["bash", f"{CST}/practice_batch.sh",
           "--count", "4", "--seed", str(seed), "--map-locked",
           "--summary", summary]
    for seat, base in sorted(our.items()):
        cmd += ["--our", f"{seat}:{base}"]
    log(f"=== {mix} x seed {seed}：{len(our)} 个我方席位，4 局轮换席位 ===")
    t0 = time.time()
    p = subprocess.run(cmd, cwd=CST)
    dt = time.time() - t0
    battles = []
    if os.path.exists(summary):
        battles = json.load(open(summary))
    okn = sum(1 for b in battles if b.get("ok"))
    log(f"=== {mix} x seed {seed}：{okn}/4 完成，用时 {dt/60:.1f} min ===")
    # 0/0和缺局不能当全胜；必须四个不同出生轮换都完成。
    complete = len(battles) == 4 and {b.get("variant") for b in battles} == {0, 1, 2, 3} and all(b.get("ok") for b in battles)
    return (0 if p.returncode == 0 and complete else 1), battles


def batch_exit_code(planned_groups, okn, total, failed):
    """批次聚合退出码：缺组/缺局/失败组任一存在即非零（护栏有效但聚合退出码必须如实）。"""
    return 0 if (planned_groups > 0 and okn == total == 4 * planned_groups and failed == 0) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=f"{CST}/tools/versions.json")
    ap.add_argument("--maps", default=None, help="逗号分隔 seed 子集；缺省全部 12 图")
    ap.add_argument("--mixes", default="M1,M2,M3",
                    help="逗号分隔；M3 在 candidate 就位前自动跳过")
    ap.add_argument("--out", default="/tmp/version_league")
    ap.add_argument("--only", default=None,
                    help="调试过滤：MIX:SEED（如 M2:20261201），只跑一项")
    args = ap.parse_args()

    manifest = json.load(open(args.manifest))
    os.makedirs(args.out, exist_ok=True)
    verify_frozen(manifest)

    seeds = [int(x) for x in (args.maps or "").split(",") if x.strip()] \
        if args.maps else manifest["seeds"]
    mixes = [m for m in args.mixes.split(",") if m.strip()]
    if args.only:
        m0, _, s0 = args.only.partition(":")
        mixes, seeds = [m0], [int(s0)]

    if not seeds or not mixes or any(m not in MIXES for m in mixes):
        ap.error("地图seed和混合均不能为空，混合只能M1/M2/M3/M4/M5")
    failed, planned_groups = 0, 0
    skipped = []
    all_battles = []
    for mix in mixes:
        if mix in ("M3", "M4", "M5") and not manifest["versions"].get("candidate"):
            log(f"{mix} 跳过：candidate 未就位；不计已完成场数")
            skipped.append(mix)
            continue
        our = {seat: seat_base(manifest, version, seat)
               for seat, version in MIXES[mix].items()}
        for seat, base in our.items():
            if not os.path.isfile(f"{resolve_base(base)}/setup.bash"):
                raise SystemExit(f"{mix} 席位 {seat} 底座缺失：{resolve_base(base)}")
        for seed in seeds:
            planned_groups += 1
            rc, battles = run_mix(seed, mix, our, args.out, args)
            all_battles.extend(battles)
            if rc != 0:
                failed += 1
    total = len(all_battles)
    okn = sum(1 for b in all_battles if b.get("ok"))
    log(f"联赛批次完成：{okn}/{total} 局 ok；失败驱动运行 {failed} 个")
    report = dict(planned=4 * planned_groups, completed=okn, returned=total,
                  failed_groups=failed, skipped_mixes=skipped, results=all_battles)
    with open(os.path.join(args.out, "league_summary.json"), "w") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
    return batch_exit_code(planned_groups, okn, total, failed)


if __name__ == "__main__":
    sys.exit(main())
