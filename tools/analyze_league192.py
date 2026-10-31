#!/usr/bin/env python3
# League 收批分析 v3：按 docs/48 §5 口径计算 M1–M5 证据；数据目录可作 argv[1]
# 修复：原版陪练 = teams 中无 binaries 记录的席位；钉→标签带席位变体意识
import json, sys
from collections import defaultdict, Counter

P = (sys.argv[1] + "/league_summary.json") if len(sys.argv) > 1 else "/home/frank/cst/build/league/league-192/league_summary.json"
d = json.load(open(P))
res = d["results"]
print("planned/completed/returned/failed:", d["planned"], d["completed"], d["returned"], d["failed_groups"])

PIN_LABEL = {
    "39557aa16d75": "champion",     # M1/M2/M3 A 位（主冠军钉）
    "98aec3f3c423": "champion",     # M4 B 位（冠军席位变体钉，契约注册）
    "dd77411fa18e": "candidate",    # M3 B 位
    "d76489d8a929": "candidate",    # M4 A 位
    "bcebb860ae2a": "old_champion", # M2 B 位
}

def label(pin12):
    return PIN_LABEL.get(pin12, "original")

def seat_label(r, seat):
    v = r["binaries"].get(seat)
    if v is None:
        return "original"
    pin = v.get("binary_sha256", "?")[:12]
    return PIN_LABEL.get(pin, "UNKNOWN_%s" % pin)

# 0) 钉分布（校验契约席位模式）
pins_by_mix_seat = defaultdict(Counter)
for r in res:
    parts = r["log_dir"].rstrip("/").split("/")
    mix = parts[-2].split("_")[1]
    for seat, v in r["binaries"].items():
        pins_by_mix_seat[(mix, seat)][v.get("binary_sha256", "?")[:12]] += 1
print("\n== 各 mix×席位 钉分布 ==")
for k in sorted(pins_by_mix_seat):
    print("  %s %s %s" % (k[0], k[1], dict(pins_by_mix_seat[k])))

# 1) 逐局：座位→标签→得分/名次
FOCAL = {"M1": "champion", "M2": "champion", "M3": "champion", "M4": "candidate", "M5": "candidate"}
OPP = {"M1": "original", "M2": "old_champion", "M3": "candidate", "M4": "champion", "M5": "original"}
rows = []
for r in res:
    parts = r["log_dir"].rstrip("/").split("/")
    mix, m = parts[-2].split("_")[1], parts[-2].split("_")[2].split(".")[0]
    teams = r["teams"]
    lab = {s: seat_label(r, s) for s in teams}
    assert all(not l.startswith("UNKNOWN") for l in lab.values()), (mix, m, lab)
    scores = {s: t["score"] for s, t in teams.items()}
    ordered = sorted(scores.items(), key=lambda kv: -kv[1])
    ranks = {s: i + 1 for i, (s, sc) in enumerate(ordered)}
    focal = FOCAL[mix]
    fseats = [s for s, l in lab.items() if l == focal]
    assert len(fseats) == 1, (mix, m, lab)
    f = fseats[0]
    others = [s for s in lab if s != f]
    rows.append(dict(
        mix=mix, map=m, case=parts[-1],
        lab=lab, fseat=f, others=others,
        fscore=scores[f], frank=ranks[f],
        others_scores={s: scores[s] for s in others},
        finv=teams[f]["invalid"],
        inv_by_lab={l: sum(teams[s]["invalid"] for s in lab if lab[s] == l) for l in set(lab.values())},
        fR=teams[f]["R"], fD=teams[f]["D"], fM=teams[f]["M"],
        ai=r["ai"][f] if f in r.get("ai", {}) else {},
    ))

# 2) 席位轮换平衡
print("\n== 焦点 bot 席位分布 ==")
for mix in ("M1", "M2", "M3", "M4", "M5"):
    c = Counter(r["fseat"] for r in rows if r["mix"] == mix)
    print("  %s %s" % (mix, dict(sorted(c.items()))))

# 3) 每 mix 聚合
print("\n== 每 mix 聚合 ==")
agg = {}
for mix in ("M1", "M2", "M3", "M4", "M5"):
    rs = [r for r in rows if r["mix"] == mix]
    if not rs:
        continue
    n = len(rs)
    fs = [r["fscore"] for r in rs]
    on = OPP[mix]
    if mix in ("M1", "M5"):
        os = [max(r["others_scores"].values()) for r in rs]   # 原版最佳（M1/M5 同池口径）
    else:
        os = [r["others_scores"][[s for s in r["others"] if r["lab"][s] == on][0]] for r in rs]
    beats = sum(1 for a, b in zip(fs, os) if a > b)
    ties = sum(1 for a, b in zip(fs, os) if a == b)
    a = dict(
        n=n, f_mean=sum(fs) / n, o_mean=sum(os) / n,
        delta=sum(x - y for x, y in zip(fs, os)) / n,
        beats=beats, ties=ties,
        f_rank=sum(r["frank"] for r in rs) / n,
        f_win=sum(1 for r in rs if r["frank"] == 1) / n,
        f_inv=sum(r["finv"] for r in rs),
        fR=sum(r["fR"] for r in rs) / n, fD=sum(r["fD"] for r in rs) / n, fM=sum(r["fM"] for r in rs) / n,
    )
    agg[mix] = a
    print("  %s: n=%d 焦点 %.2f (R%.1f/D%.1f/M%.1f) rank%.2f win%.0f%% | 对手(%s) %.2f | Δ=%+.2f 得胜%d 平%d | invalid 焦点%d" % (
        mix, n, a["f_mean"], a["fR"], a["fD"], a["fM"], a["f_rank"], a["f_win"] * 100,
        on, a["o_mean"], a["delta"], a["beats"], a["ties"], a["f_inv"]))
    inv_lab = Counter()
    for r in rs:
        for l, c in r["inv_by_lab"].items():
            inv_lab[l] += c
    print("     invalid 按标签: %s" % dict(inv_lab))

# 4) 席位效应
print("\n== 焦点 bot 按席位均分 ==")
for mix in ("M1", "M2", "M3", "M4", "M5"):
    by = defaultdict(list)
    for r in rows:
        if r["mix"] == mix:
            by[r["fseat"]].append(r["fscore"])
    print("  %s %s" % (mix, {s: "%.2f(n=%d)" % (sum(v) / len(v), len(v)) for s, v in sorted(by.items())}))

# 5) 每图×每 mix
print("\n== 每图×每 mix 焦点均分/均名次 ==")
maps = sorted(set(r["map"] for r in rows))
print("  map       " + "".join("%12s" % mix for mix in ("M1", "M2", "M3", "M4", "M5")))
for m in maps:
    line = "  %-10s" % m
    for mix in ("M1", "M2", "M3", "M4", "M5"):
        rs = [r for r in rows if r["mix"] == mix and r["map"] == m]
        if not rs:
            line += " " * 12
            continue
        fs = sum(r["fscore"] for r in rs) / len(rs)
        rk = sum(r["frank"] for r in rs) / len(rs)
        line += "%6.2f/r%.1f " % (fs, rk)
    print(line)

# 6) 同局两两对比
print("\n== 同局两两对比 ==")
for mix in ("M2", "M3", "M4"):
    rs = [r for r in rows if r["mix"] == mix]
    on = OPP[mix]
    beats = ties = 0
    for r in rs:
        os = r["others_scores"][[s for s in r["others"] if r["lab"][s] == on][0]]
        if r["fscore"] > os:
            beats += 1
        elif r["fscore"] == os:
            ties += 1
    print("  %s: 焦点>%s %d/%d（平 %d）" % (mix, on, beats, len(rs), ties))

# 7) 钉出现次数（全量）
c = Counter()
for r in res:
    for seat, v in r["binaries"].items():
        c[label(v.get("binary_sha256", "?")[:12])] += 1
print("\n== 钉出现次数 ==", dict(c))

# 8) rank4 清单
print("\n== 焦点 bot rank4 的局 ==")
bad = [r for r in rows if r["frank"] == 4]
print(len(bad), "局")
from collections import Counter as _C
print("  按 mix:", dict(_C(r["mix"] for r in bad)))
for r in bad:
    print("  %s %s %s seat=%s f=%.2f others=%s" % (
        r["mix"], r["map"], r["case"], r["fseat"], r["fscore"],
        {r["lab"][s]: v for s, v in r["others_scores"].items()}))

# 9) AI 调用统计（manual：stage2 每场须真实 AI 成功调用）
print("\n== 焦点 bot AI 调用（每 mix 合计） ==")
for mix in ("M1", "M2", "M3", "M4", "M5"):
    calls = succ = fail = 0
    for r in rows:
        if r["mix"] != mix:
            continue
        a = r["ai"]
        calls += a.get("calls", 0)
        succ += a.get("success", 0)
        fail += a.get("fail", 0)
    print("  %s: calls=%d success=%d fail=%d" % (mix, calls, succ, fail))

json.dump(dict(agg=agg, rows=rows), open("/tmp/league192_analysis.json", "w"))
print("\ndone")
