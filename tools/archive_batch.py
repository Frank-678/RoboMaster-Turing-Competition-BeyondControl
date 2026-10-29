#!/usr/bin/env python3
"""批次对局归档（10 件套 + 密钥扫描 + README 战报表），log 分支上传前置步骤。
用法：
  python3 tools/archive_batch.py --summary <summary.json> --to <归档目录> \\
      [--configs <驱动 log 目录，取 config_<vid>.json 变体存档>] [--botlogs <驱动 log 目录，取我方 bot<Seat>.log>]
每局（包括已有session的失败局）从 ~/.rmtc-practice/<session>/ 拷贝 10 件套到
<to>/<session>/；缺件跳过并在 archive.json 里记录 missing。归档前全量密钥扫描，
发现疑似密钥立即中止。README.md 生成战报表（协议沿用 rrh10 批次）。
"""
import argparse
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path
from datetime import datetime
from zoneinfo import ZoneInfo

PIECES = ["ai.jsonl", "communication.jsonl", "config.json", "frames.jsonl",
          "referee.log", "score.json", "scoring.log",
          "bot_B.log", "bot_C.log", "bot_D.log"]
KEY_RE = re.compile(r"sk-[A-Za-z0-9_-]{20,}")  # 检查疑似密钥形状；不只识别某一枚测试密钥。


def scan_keys(path):
    """目录与单文件都检查；os.walk(file)会静默漏掉summary。"""
    paths = [path] if os.path.isfile(path) else [os.path.join(root, name)
              for root, _dirs, files in os.walk(path) for name in files]
    hits = []
    for current in paths:
        try:
            text = Path(current).read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if KEY_RE.search(text):
            hits.append(current)
    return hits


def load_summary(path):
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict):
        data = data.get("results", [data])
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", required=True)
    ap.add_argument("--to", required=True)
    ap.add_argument("--source", default=os.path.expanduser("~/.rmtc-practice"))
    ap.add_argument("--configs", default=None, help="驱动 log 目录（config_<vid>.json）")
    ap.add_argument("--botlogs", default=None, help="驱动 log 目录（我方 bot<Seat>.log）")
    args = ap.parse_args()

    entries = load_summary(args.summary)
    ok = [e for e in entries if e.get("ok")]
    if scan_keys(args.summary):
        raise SystemExit("摘要含疑似密钥，未复制或发布。")
    # 旧驱动一个目录只留最后一局bot日志；多局归档禁止把它冒充每局日志。
    if args.botlogs and len(entries) > 1 and any(not e.get("log_dir") for e in entries):
        raise SystemExit("多局旧摘要缺每局log_dir，不能证明--botlogs归属；请先补场次映射。")
    os.makedirs(args.to, exist_ok=True)
    meta = {"summary": os.path.abspath(args.summary), "to": os.path.abspath(args.to),
            "ts": datetime.now(ZoneInfo("Asia/Shanghai")).isoformat(), "battles": len(entries),
            "completed": len(ok), "failed": len(entries) - len(ok),
            "failures": [e for e in entries if not e.get("ok")],
            "sessions": [], "missing": {}, "key_scan": None}
    rows = []
    for e in entries:
        session = e.get("session")
        if not session:
            meta["missing"][f"variant-{e['variant']}"] = ["session"]
            continue
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
        config_root = e.get("log_dir") or args.configs
        if config_root:
            vcfg = e.get("config_path") or os.path.join(config_root, f"config_{e['variant']}.json")
            if os.path.isfile(vcfg):
                shutil.copy2(vcfg, os.path.join(dst, "config_variant.json"))
            else:
                missing.append(f"config_{e['variant']}.json")
        bot_root = e.get("log_dir") or args.botlogs
        if bot_root:
            for seat in "ABCD":
                blog = os.path.join(bot_root, f"bot{seat}.log")
                if os.path.isfile(blog):
                    shutil.copy2(blog, os.path.join(dst, f"bot_{seat}_ours.log"))
        meta["sessions"].append(session)
        if missing:
            meta["missing"][session] = missing
        t = e.get("teams", {})
        rows.append({"variant": e["variant"], "session": session,
                     "teams": t, "ai": e.get("ai", {}), "ok": bool(e.get("ok")), "error": e.get("error")})

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
             f"- 计划记录：{len(entries)}；完成：{len(ok)}；失败：{len(entries)-len(ok)}；归档目录：`{os.path.basename(os.path.abspath(args.to))}`",
             f"- 密钥扫描：{'通过' if not hits else '失败'}；缺件：{sum(len(v) for v in meta['missing'].values())}",
             f"- 机器：{os.uname().nodename}；并行：单局 4 席位同场并行，局间串行（batch_practice.py 串行调度）",
             ""]
    for r in rows:
        lines.append(f"## variant {r['variant']} · {r['session']}")
        lines.append("")
        lines.append(f"状态：{'完成' if r['ok'] else '失败'}；错误：{r['error'] or '无'}")
        lines.append("")
        lines.append("| 席位 | score | R | D | M | hp | invalid |")
        lines.append("|---|---|---|---|---|---|---|")
        for seat, v in sorted(r["teams"].items()):
            lines.append(f"| {seat} | {v['score']} | {v['R']} | {v['D']} | "
                         f"{v['M']} | {v['hp']} | {v['invalid']} |")
        lines.append("")
    Path(args.to, "README.md").write_text("\n".join(lines), encoding="utf-8")
    shutil.copy2(args.summary, os.path.join(args.to, "batch_summary.json"))
    Path(args.to, "archive.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1))
    print(f"归档完成：{len(meta['sessions'])} 局 -> {args.to}（密钥扫描通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
