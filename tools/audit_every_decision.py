#!/usr/bin/env python3
"""Audit EVERY decision in RMTC archived stage-2 sessions, with 2-5 tick pre-death windows.

Offline referee truth is allowed ONLY for diagnosis; never feeds contestant decisions.
This does NOT establish counterfactual survival. Use state-fork original referee tests.
"""
import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path

ACTION = re.compile(
    r"\btick=(\d+)\b.*?\bpos=\[(-?\d+),(-?\d+)\].*?"
    r"\bmode=(\S+).*?\bsent=(\S+).*?\bdirection=(\S+).*?\breason=(\S+)"
)
DELTAS = ((0,-1),(1,0),(0,1),(-1,0))
def parse_bot(path):
    out={}
    for line in path.read_text(encoding="utf-8",errors="replace").splitlines():
        m=ACTION.search(line)
        if m:
            t,x,y,mode,act,direction,reason=m.groups()
            out[int(t)]={"pos":(int(x),int(y)),"mode":mode,"action":act,
                         "direction":direction,"reason":reason}
    return out

def team(frame,name="A"):
    return next(t for t in frame["teams"] if t["team"]==name)
def opponents(frame):
    return [x for x in frame["map_view"]["robots"] if x["team"]!="A" and x["alive"]]
def pressure(frame, pos):
    """Hindsight referee positioning, NOT proof that the robot knew or enemy fired."""
    near=[]
    for e in opponents(frame):
        p=e["position"]
        if (p[0]==pos[0] and 0<abs(p[1]-pos[1])<=2) or (
           p[1]==pos[1] and 0<abs(p[0]-pos[0])<=2):
            near.append(e["team"])
    return near
def adj_options(frame,pos):
    mv=frame["map_view"];m=mv["map"]
    blocks={tuple(x) for x in mv["obstacles"]}
    occupied={tuple(x["position"]) for x in opponents(frame)}
    ans=[]
    for idx,(dx,dy) in enumerate(DELTAS):
        cell=(pos[0]+dx,pos[1]+dy)
        if 0<=cell[0]<m["width"] and 0<=cell[1]<m["height"] and cell not in blocks and cell not in occupied:
            ans.append({"dir":"URDL"[idx],"cell":cell,"exposed_if_enemies_still":pressure(frame,cell)})
    return ans

def analyse(session, writer, deaths_writer):
    frames=[json.loads(x) for x in (session/"frames.jsonl").read_text(encoding="utf-8").splitlines() if x.strip()]
    log=session/"bot_A_ours.log"
    if not log.exists():
        raise FileNotFoundError(str(log))
    acts=parse_bot(log)
    assert all(f["tick"]==i+1 for i,f in enumerate(frames)), "nonsequential frames"
    assert len(frames)==500, "incomplete game"
    rows=[]
    dead_started=False
    death_ticks=[]
    reason_counts=Counter()
    for i,after in enumerate(frames):
        tick=after["action_tick"]
        a=acts.get(tick)
        if a is None:
            a={"pos":(None,None),"mode":"unknown","action":"MISSING",
               "direction":"?","reason":"missing_action_log"}
        pre=frames[i-1] if i else None
        before=team(pre) if pre else None
        now=team(after)
        risk=pressure(pre,a["pos"]) if pre and a["pos"][0] is not None else []
        legal=adj_options(pre,a["pos"]) if pre and a["pos"][0] is not None else []
        r={
            "session":session.name, "tick":tick,"pre_x":a["pos"][0],"pre_y":a["pos"][1],
            "pre_hp":before["hp"] if before else "", "post_hp":now["hp"],
            "pre_R":before["resource_value"] if before else "", "post_R":now["resource_value"],
            "pre_D":before["effective_damage"] if before else "", "post_D":now["effective_damage"],
            "action":a["action"],"direction":a["direction"],"reason":a["reason"],
            "enemy_attack_line":",".join(risk),"free_adj":len(legal),
            "hindsight_ray_free_adj":sum(not x["exposed_if_enemies_still"] for x in legal),
            "death_started":int(not now["alive"] and (before is None or before["alive"])),
            "post_alive":int(now["alive"]),
        }
        rows.append(r);writer.writerow(r);reason_counts[a["reason"]]+=1
        if r["death_started"]:death_ticks.append(tick)
    for t in death_ticks:
        death=rows[t]
        for lead in (2,3,4,5):
            i=t-lead
            if i<0:continue
            earlier=rows[i]
            deaths_writer.writerow({
                "session":session.name,"death_action_tick":t,"lead":lead,
                "earlier_tick":i,"earlier_hp":earlier["pre_hp"],"earlier_action":earlier["action"],
                "earlier_reason":earlier["reason"],"earlier_enemy_attack_line":earlier["enemy_attack_line"],
                "earlier_free_adj":earlier["free_adj"],
                "earlier_hindsight_ray_free_adj":earlier["hindsight_ray_free_adj"],
                "terminal_action":death["action"],"terminal_reason":death["reason"],
                "terminal_pre_hp":death["pre_hp"],"terminal_pre_R":death["pre_R"],
                "terminal_pre_D":death["pre_D"]})
    return {"session":session.name,"decisions":len(rows),"deaths":len(death_ticks),
            "death_action_ticks":death_ticks,
            "missing_actions":sum(r["action"]=="MISSING" for r in rows),
            "attack_deaths":sum(rows[t]["action"]=="attack" for t in death_ticks),
            "postcap_attacks":sum(r["action"]=="attack" and r["pre_D"]!="" and r["pre_D"]>=500 for r in rows),
            "lowhp_attacks":sum(r["action"]=="attack" and r["pre_hp"]!="" and r["pre_hp"]<=20 for r in rows),
            "R":team(frames[-1])["resource_value"],"D":team(frames[-1])["effective_damage"],
            "M":team(frames[-1])["mission_completed"],"score":team(frames[-1])["score"],
            "frequent_reasons":reason_counts.most_common(8)}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("batches",nargs="+",type=Path,help="archived batch roots containing practice-*/frames.jsonl")
    ap.add_argument("--out",required=True,type=Path)
    args=ap.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    fields=["session","tick","pre_x","pre_y","pre_hp","post_hp","pre_R","post_R",
            "pre_D","post_D","action","direction","reason","enemy_attack_line","free_adj",
            "hindsight_ray_free_adj","death_started","post_alive"]
    wfields=["session","death_action_tick","lead","earlier_tick","earlier_hp","earlier_action",
             "earlier_reason","earlier_enemy_attack_line","earlier_free_adj",
             "earlier_hindsight_ray_free_adj","terminal_action","terminal_reason",
             "terminal_pre_hp","terminal_pre_R","terminal_pre_D"]
    results=[]
    with (args.out/"every_decision.csv").open("w",newline="",encoding="utf-8") as f, \
         (args.out/"death_lookback_2_to_5.csv").open("w",newline="",encoding="utf-8") as g:
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
        d=csv.DictWriter(g,fieldnames=wfields);d.writeheader()
        for batch in args.batches:
            for session in sorted(batch.glob("practice-*")):
                if (session/"frames.jsonl").exists():
                    results.append(analyse(session,w,d))
    if not results:raise SystemExit("no complete practice-* sessions")
    (args.out/"summary.json").write_text(json.dumps(results,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"complete_games":len(results),"audited_decisions":sum(x["decisions"] for x in results),
                      "deaths":sum(x["deaths"] for x in results)},ensure_ascii=False))
if __name__=="__main__":main()
