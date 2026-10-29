#!/usr/bin/env python3
"""批次对局归档（10 件套 + 密钥扫描 + README 战报表），log 分支上传前置步骤。
用法：
  python3 tools/archive_batch.py --summary <summary.json> --to <归档目录> \\
      [--configs <驱动 log 目录，取 config_<vid>.json 变体存档>] [--botlogs <驱动 log 目录，取我方 bot<Seat>.log>]
每局（summary 中有 session 且 ok）从 ~/.rmtc-practice/<session>/ 拷贝 10 件套到
<to>/<session>/；缺件跳过并在 archive.json 里记录 missing。归档前全量密钥扫描，
发现 sk-cbc 立即中止（合规硬约束）。README.md 生成战报表（协议沿用 rrh10 批次）。
"""
import argparse
import json
import os
import re
import shutil
import sys
import time

PIECES = ["ai.jsonl", "communication.jsonl", "config.json", "frames.jsonl",
          "referee.log", "score.json", "scoring.log",
          "bot_B.log", "bot_C.log", "bot_D.log"]
KEY_RE = re.compile(r"sk-cbc[0-9a-f]{8,}")  # 真实密钥形状（sk-cbc+32hex）；裸前缀不算命中


def scan_keys(path):
    hits = []
    for root, _dirs, files in os.walk(path):
        for name in files:
            p = os.path.join(root, name)
            try:
                text = open(p, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            if KEY_RE.search(text):
                hits.append(p)
    return hits


def load_summary(path):
    data = json.load(open(path))
    if isinstance(data, dict):
        data = data.get("results", [data])
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", required=True)
    ap.add_argument("--to", required=True)
    ap.add_argument("--source", default="/home/frank/.rmtc-practice")
    ap.add_argument("--configs", default=None, help="驱动 log 目录（config_<vid>.json）")
    ap.add_argument("--botlogs", default=None, help="驱动 log 目录（我方 bot<Seat>.log）")
    args = ap.parse_args()

    entries = load_summary(args.summary)
    ok = [e for e in entries if e.get("ok")]
    os.makedirs(args.to, exist_ok=True)
    meta = {"summary": os.path.abspath(args.summary), "to": os.path.abspath(args.to),
            "ts": time.strftime("%Y-%m-%d %H:%M:%S +0800"), "battles": len(ok),
            "sessions": [], "missing": {}, "key_scan": None}
    rows = []
    for e in ok:
        session = e["session"]
        src = os.path.join(args.source, session)
        dst = os.path.join(args.to, session)
        if not os.path.isdir(src):
            meta["missing"][session] = ["run_dir"]
            continue
        os.makedirs(dst, exist_ok=True)
        missing = []
        for piece in PIECES:
            p = os.path.join(src, piece)
            if os.path.isfile(p):
                shutil.copy2(p, os.path.join(dst, piece))
            else:
                missing.append(piece)
        if args.configs:
            vcfg = os.path.join(args.configs, f"config_{e['variant']}.json")
            if os.path.isfile(vcfg):
                shutil.copy2(vcfg, os.path.join(dst, "config_variant.json"))
            else:
                missing.append(f"config_{e['variant']}.json")
        if args.botlogs:
            for seat in "ABCD":
                blog = os.path.join(args.botlogs, f"bot{seat}.log")
                if os.path.isfile(blog):
                    shutil.copy2(blog, os.path.join(dst, f"bot_{seat}_ours.log"))
        meta["sessions"].append(session)
        if missing:
            meta["missing"][session] = missing
        t = e.get("teams", {})
        rows.append({"variant": e["variant"], "session": session,
                     "teams": t, "ai": e.get("ai", {})})

    # 归档前密钥扫描（含 summary 文件本身），发现即中止。
    hits = scan_keys(args.to) + scan_keys(args.summary)
    meta["key_scan"] = {"ok": not hits, "hits": len(hits)}
    if hits:
        print(f"密钥扫描发现 {len(hits)} 处命中，中止归档：")
        for h in hits:
            print(f"  {h}")
        sys.exit(1)

    # README 战报表（协议：原始分/R/D/M/hp/invalid 按席位）。
    lines = ["# 批次对局归档", "",
             f"- 摘要源：`{os.path.basename(args.summary)}`（{meta['ts']}）",
             f"- 完成对局：{len(ok)}；归档目录：`{os.path.basename(os.path.abspath(args.to))}`",
             f"- 密钥扫描：{'通过' if not hits else '失败'}；缺件：{sum(len(v) for v in meta['missing'].values())}",
             ""]
    for r in rows:
        lines.append(f"## variant {r['variant']} · {r['session']}")
        lines.append("")
        lines.append("| 席位 | score | R | D | M | hp | invalid |")
        lines.append("|---|---|---|---|---|---|---|")
        for seat, v in sorted(r["teams"].items()):
            lines.append(f"| {seat} | {v['score']} | {v['R']} | {v['D']} | "
                         f"{v['M']} | {v['hp']} | {v['invalid']} |")
        lines.append("")
    open(os.path.join(args.to, "README.md"), "w", encoding="utf-8").write("\n".join(lines))
    json.dump(meta, open(os.path.join(args.to, "archive.json"), "w"),
              ensure_ascii=False, indent=1)
    print(f"归档完成：{len(meta['sessions'])} 局 -> {args.to}（密钥扫描通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
