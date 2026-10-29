"""联赛场景生成：几何、资源布局、出生点和时序分别标记；不给选手任何全图输入。"""
from collections import deque
import random

SEATS = "ABCD"
FAMILIES = ["sparse", "rooms", "bridge", "snake", "ring", "dead_ends", "wide_lanes",
            "asymmetric", "center_race", "resource_bias", "dynamic_cut", "late_empty"]


def distances(start, walls, width=24):
    pending, found = deque([start]), {start: 0}
    while pending:
        x, y = pending.popleft()
        for cell in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= cell[0] < width and 0 <= cell[1] < width and cell not in walls and cell not in found:
                found[cell] = found[(x, y)] + 1
                pending.append(cell)
    return found


def fixture(family, seed, ticks=500):
    """生成一张冻结后可手工改摆的合法夹具；不强行打通四条外围高速路。"""
    rng, width, walls = random.Random(seed), 24, set()
    spawns = [(1, 1), (22, 1), (22, 22), (1, 22)]
    if family in ("rooms", "bridge", "dynamic_cut"):
        walls = {(12, y) for y in range(width) if y not in ([6, 18] if family == "rooms" else [12])}
        if family == "rooms":
            walls |= {(x, 12) for x in range(width) if x not in (5, 18)}
    elif family == "snake":
        for index, y in enumerate(range(4, 22, 4)):
            walls |= {(x, y) for x in range(width) if x != (22 if index % 2 == 0 else 1)}
    elif family == "ring":
        walls = {(x, y) for x in range(6, 18) for y in range(6, 18)}
    elif family == "dead_ends":
        free_cells = {(x, 12) for x in range(width)}
        for index, x in enumerate(range(1, 23, 3)):
            free_cells |= {(x, y) for y in (range(3, 13) if index % 2 == 0 else range(12, 22))}
        walls = {(x, y) for x in range(width) for y in range(width)} - free_cells
        spawns = [(1, 12), (22, 12), (1, 3), (22, 21)]
    elif family == "wide_lanes":
        walls = {(x, y) for x in (6, 7, 14, 15) for y in range(2, 22)}
    elif family == "center_race":
        spawns = [(8, 8), (15, 8), (15, 15), (8, 15)]
    elif family in ("sparse", "asymmetric"):
        attempts = 35 if family == "sparse" else 190
        for _ in range(attempts):
            cell = (rng.randrange(width), rng.randrange(width))
            if cell not in spawns:
                trial = walls | {cell}
                reachable = distances(spawns[0], trial)
                if all(spawn in reachable for spawn in spawns):
                    walls = trial
    elif family not in ("resource_bias", "late_empty"):
        raise ValueError("unknown league map family: " + family)
    reachable = distances(spawns[0], walls)
    if any(spawn not in reachable for spawn in spawns):
        raise ValueError("generated fixture has disconnected spawns")
    free = sorted(set(reachable) - set(spawns))
    rng.shuffle(free)
    if family == "resource_bias":
        free.sort(key=lambda cell: cell[0])
    if family == "center_race":
        free.sort(key=lambda cell: abs(cell[0] - 12) + abs(cell[1] - 12))
    resources = [{"position": list(cell), "value": 20 if index < 4 else 10} for index, cell in enumerate(free[:16])]
    initial_tick = 200 if family == "late_empty" else 0
    if family == "late_empty":
        resources = []
    cars = {seat: {"position": list(spawns[index]), "hp": 100} for index, seat in enumerate(SEATS)}
    if initial_tick:
        for car in cars.values():
            car.update(R=40, D=300, normal=2, high=1, completed=["normal", "high"])
    missions = [{"id": "normal", "type": "normal_resources", "target": 2},
                {"id": "high", "type": "high_resources", "target": 1},
                {"id": "reach", "type": "reach", "target": 1, "position": list(free[20])}]
    events = []
    # 常见八类都有机会；动态切断和叠加场景保留明确标签，不声称穷举组合。
    def add(kind, start, duration, **fields):
        event = dict(kind=kind, start=start, end=start + duration, source=f"fixture {family} {kind} start={start}", **fields)
        events.append(dict(id=kind, notice_tick=max(initial_tick, start - 10),
                           delivery_tick=max(initial_tick, start - 8), event=event))
    offset = initial_tick
    add("explosion", offset + 30, 1, area=[9, 9, 15, 15], amount=35)
    add("illusion", offset + 45, 25, area=[2, 2, 18, 18])
    add("heal", offset + 85, 25, points=[list(free[16])], amount=10, effect_ticks=10)
    add("fortress", offset + 120, 65, area=[9, 9, 14, 14], amount=50, defended_damage=10, capture_ticks=8)
    add("split", offset + (31 if family == "dynamic_cut" else 225), 30, points=[[12, 12]])
    add("chili", offset + 275, 30, points=[list(free[17]), list(free[18])], amount=50)
    add("reflect", offset + 330, 20, amount=20)
    add("vulnerability", offset + 390, 30, trigger_end=offset + 410, multiplier=2)
    # 500Tick晚期场只保留有可能在本场生效的事件，不偷把窗口截成新规则。
    events = [event for event in events if event["event"]["start"] < ticks]
    scene = dict(schema=1, name=f"{family}-{seed}", seed=seed, stage=2, tick_limit=ticks, qualified=True,
                 map=dict(width=width, height=width, vision_radius=4, walls=[list(c) for c in sorted(walls)],
                          spawns=[list(c) for c in spawns]),
                 initial=dict(tick=initial_tick, resources=resources, cars=cars), missions=missions, events=events,
                 agents={}, run=dict(until_tick=ticks, always=[dict(path=f"/world/cars/{s}/invalid", op="eq", value=0) for s in SEATS]))
    degrees = [sum(n in reachable for n in ((x-1,y),(x+1,y),(x,y-1),(x,y+1))) for x, y in reachable]
    metrics = dict(reachable_cells=len(reachable), wall_cells=len(walls), dead_ends=sum(d == 1 for d in degrees),
                   narrow_cells=sum(d == 2 for d in degrees), resource_total=sum(r["value"] for r in resources),
                   spawn_distances=[[distances(a, walls)[b] for b in spawns] for a in spawns])
    return scene, metrics


def make_suite(labels, anchor, versions, seed=20261029, ticks=500):
    """12局面×4焦点席位×3对手组合；同一清单给所有候选，不因胜负更换地图。"""
    cases = []
    mixes = {"economy": ["collector", "opportunist", "defender"],
             "pressure": ["berserker", "sentry", "trickster"], "archive": ["$anchor"] * 3}
    for index, family in enumerate(FAMILIES):
        scene, metrics = fixture(family, seed + index, ticks)
        for focal in SEATS:
            for mix, opponents in mixes.items():
                agents, iterator = {}, iter(opponents)
                for seat in SEATS:
                    role = "$focal" if seat == focal else next(iterator)
                    agents[seat] = dict(type="external", version=role, event_delivery="oracle") if role.startswith("$") else dict(type="npc", style=role, event_delivery="oracle")
                cases.append(dict(id=f"{family}-{focal}-{mix}", map_id=scene["name"], family=family, focal=focal,
                                  mix=mix, metrics=metrics, scene={**scene, "agents": agents}))
    return dict(schema=1, candidates=labels, anchor=anchor, versions=versions, cases=cases,
                scope="offline fixtures, 12 map/resource/time families; not official practice matches")
