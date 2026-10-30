#!/usr/bin/env python3
"""Read-only all-Tick auditor for RMTC leagues M1-M4 plus M5 candidate.

Reads complete archived botA.log plus summary_M{1,2,3,4,5}_*.json under two roots.
Produces evidence-aligned EVERY-decision and 2..5-Tick lookback ledgers.
This is NOT a causal replay, nor a death/HP audit without referee frames.
Never uses referee hidden state to choose an in-game action.
"""
import argparse
import csv
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

ACTION = re.compile(
    r"\btick=(\d+)\b.*?\bpos=\[(-?\d+),(-?\d+)\].*?"
    r"\bmode=(\S+).*?\bsent=(\S+).*?\bdirection=(\S+).*?\breason=(\S+)"
)
NAV = re.compile(r"\bnavigation=(\{[^}]*\})")
LAST = re.compile(r"\blast_action=(null|\{[^}]*\})")
TRIGGERS = {
    "defend_to_attack": lambda a,b:a and a["reason"]=="defend_lethal_threat"
                               and b["reason"]=="attack_opportunity",
    "approach_to_attack": lambda a,b:a and a["reason"]=="approach_enemy"
                                 and b["reason"]=="attack_opportunity",
    "reach_to_attack": lambda a,b:a and a["reason"]=="reach_mission"
                              and b["reason"]=="attack_opportunity",
    "post_dead": lambda a,b:a and a["reason"]=="dead" and b["reason"]!="dead",
}
def read_json(p):
    return json.loads(p.read_text(encoding="utf-8"))

def parse_log(p):
    rows, duplicates, session_ids = {}, [], set()
    for line_no,line in enumerate(p.read_text(encoding="utf-8",errors="replace").splitlines(),1):
        m=ACTION.search(line)
        if not m:continue
        tick,x,y,mode,action,direction,reason=m.groups()
        tick=int(tick)
        nav=NAV.search(line)
        last=LAST.search(line)
        sid=re.search(r"\bsession=(\S+)",line)
        if sid:session_ids.add(sid.group(1))
        rec={"tick":tick,"x":int(x),"y":int(y),"mode":mode,
             "action":action,"direction":direction,"reason":reason,
             "log_line":line_no,
             "navigation":json.loads(nav.group(1)) if nav else {},
             "last_action":json.loads(last.group(1)) if last else None}
        if tick in rows:
            duplicates.append({"tick":tick,"prior_line":rows[tick]["log_line"],"new_line":line_no})
            # An ambiguous tick must remain explicitly ambiguous: do not silently overwrite.
            continue
        rows[tick]=rec
    return rows,duplicates,sorted(session_ids)

def get_cases(root,kind):
    cases=[]
    for f in sorted(root.glob(f"summary_{kind}_*.json")):
        data=read_json(f)
        if not isinstance(data,list):raise ValueError(f"unexpected summary shape: {f}")
        for entry in data:
            if not entry.get("ok"):continue
            name=Path(entry["log_dir"]).name
            parent=Path(entry["log_dir"]).parent.name
            p=root/parent/name/"botA.log"
            cases.append({"summary":str(f),"log":p,"entry":entry,
                          "seed":f.stem.split("_")[-1],"variant":entry["variant"]})
    return cases

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--m1",type=Path,required=True,help="log/league-192-2026-10-30")
    ap.add_argument("--m5",type=Path,required=True,help="log/league-m5-2026-10-30")
    ap.add_argument("--out",type=Path,required=True)
    a=ap.parse_args()
    a.out.mkdir(parents=True,exist_ok=True)
    m1=get_cases(a.m1,"M1")
    m5=get_cases(a.m5,"M5")
    ref={case["entry"]["config_sha256"]:case for case in m1}
    groups=[('M1',m1),('M2',get_cases(a.m1,'M2')),
            ('M3',get_cases(a.m1,'M3')),('M4',get_cases(a.m1,'M4')),
            ('M5',m5)]
    decisions, windows, pairs, inventory=[],[],[],[]
    cached={}
    for role,cases in groups:
        for case in cases:
            e=case["entry"]
            log=case["log"]
            if not log.is_file():
                inventory.append({"role":role,"seed":case["seed"],"variant":case["variant"],
                                  "error":"log_missing","log":str(log)})
                continue
            text=log.read_bytes()
            actions, duplicates,sessions=parse_log(log)
            sid=e.get("session")
            wrong_sessions=[s for s in sessions if s!=sid]
            missing=[t for t in range(500) if t not in actions]
            actlist=[actions[t] for t in sorted(actions) if 0<=t<500]
            n=Counter(x["reason"] for x in actlist)
            old=ref.get(e["config_sha256"]) if role=="M5" else None
            inventory.append({"role":role,"seed":case["seed"],"variant":case["variant"],
                "session":sid,"log":str(log),"log_sha256":hashlib.sha256(text).hexdigest(),
                "config_sha256":e["config_sha256"],"binary_sha256":e["binaries"]["A"]["binary_sha256"],
                "ticks_present":len(actlist),"missing_ticks":missing,
                "duplicate_ticks":duplicates,"wrong_sessions":wrong_sessions,
                "terminal_score":e["teams"]["A"]["score"],"terminal_R":e["teams"]["A"]["R"],
                "terminal_D":e["teams"]["A"]["D"],"terminal_M":e["teams"]["A"]["M"],
                "dead_reason_ticks":n["dead"],"defend_reason_ticks":n["defend_lethal_threat"]})
            cached[(role,e["config_sha256"])]=actions
            for tick in range(500):
                row=actions.get(tick)
                prev=actions.get(tick-1)
                if row is None:
                    decisions.append({"role":role,"seed":case["seed"],"variant":case["variant"],
                        "session":sid,"tick":tick,"status":"MISSING"})
                    continue
                nav=row["navigation"];last=row["last_action"]
                status="OK" if not duplicates and not wrong_sessions else "AMBIGUOUS_LOG"
                if tick>0 and last and last.get("tick") not in (tick-1,None):
                    status="LAST_ACTION_TICK_MISMATCH"
                decisions.append({"role":role,"seed":case["seed"],"variant":case["variant"],
                    "session":sid,"tick":tick,"status":status,"x":row["x"],"y":row["y"],
                    "action":row["action"],"direction":row["direction"],"reason":row["reason"],
                    "stagnant_ticks":nav.get("stagnant_ticks"),"recovery":nav.get("recovering"),
                    "last_action_tick":last.get("tick") if isinstance(last,dict) else "",
                    "last_action_moved":last.get("moved") if isinstance(last,dict) else "",
                    "predecessor_reason":prev["reason"] if prev else "MISSING"})
                for label,predicate in TRIGGERS.items():
                    if not predicate(prev,row):continue
                    for lead in (2,3,4,5):
                        earlier=actions.get(tick-lead)
                        windows.append({"role":role,"seed":case["seed"],"variant":case["variant"],
                            "session":sid,"trigger":label,"trigger_tick":tick,"lookback":lead,
                            "earlier_tick":tick-lead,"earlier_status":"PRESENT" if earlier else "MISSING",
                            "earlier_action":earlier["action"] if earlier else "",
                            "earlier_reason":earlier["reason"] if earlier else "",
                            "earlier_pos":f'{earlier["x"]},{earlier["y"]}' if earlier else "",
                            "trigger_action":row["action"],"trigger_reason":row["reason"]})
            if role=="M5":
                old=ref.get(e["config_sha256"])
                if old:
                    oa=old["entry"]["teams"]["A"];ca=e["teams"]["A"]
                    old_rows=cached.get(("M1",e["config_sha256"]),{})
                    # Missing t0 is explicitly not counted as proof of first divergence.
                    comparable=[t for t in range(500) if t in old_rows and t in actions]
                    diff=[t for t in comparable if
                        (old_rows[t]["action"],old_rows[t]["direction"])!=
                        (actions[t]["action"],actions[t]["direction"])]
                    pairs.append({"seed":case["seed"],"variant":case["variant"],
                        "config_sha256":e["config_sha256"],"same_config_sha":True,
                        "compared_ticks":len(comparable),"missing_comparison_ticks":500-len(comparable),
                        "first_action_divergence_tick":diff[0] if diff else "",
                        "note":"Same config hash does not prove identical future RNG stream or causal effect.",
                        "score_M1":oa["score"],"score_M5":ca["score"],
                        "R_M1":oa["R"],"R_M5":ca["R"],"D_M1":oa["D"],"D_M5":ca["D"],
                        "M_M1":oa["M"],"M_M5":ca["M"]})
    def dump(name,rows):
        file=a.out/name
        if not rows:
            file.write_text("",encoding="utf-8");return
        fields=list(dict.fromkeys(k for r in rows for k in r))
        with file.open("w",newline="",encoding="utf-8") as h:
            w=csv.DictWriter(h,fieldnames=fields)
            w.writeheader();w.writerows(rows)
    dump("every_decision.csv",decisions)
    dump("two_to_five_tick_windows.csv",windows)
    dump("paired_m1_m5.csv",pairs)
    (a.out/"evidence_manifest.json").write_text(json.dumps(inventory,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"cases_by_group":{k:len(v) for k,v in groups},"decision_positions":len(decisions),
                      "complete_action_rows":sum(x.get("status")!="MISSING" for x in decisions),
                      "review_windows":len(windows),"paired":len(pairs),
                      "caution":"Action-only logs cannot establish HP, death, resource first sighting, or counterfactual survival."},
                     ensure_ascii=False,indent=2))
if __name__=="__main__":
    main()
