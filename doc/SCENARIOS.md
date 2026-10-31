# 短场景、断点与分叉测试

不同历史源码同场对打使用[LEAGUE.md](LEAGUE.md)的独立worker；`external` agent仅供联赛入口，普通短场景仍使用strategy/npc/script。

入口：`bash tests/scripts/scenarios.sh 场景.json --out 结果前缀`。直接摆好第200 Tick的车辆、资源与事件，只跑需要验证的一段。它只链接离线策略和自建裁判，不启动ROS、不调用模型、不修改`practice/`。

## 先跑四个例子

```bash
# A只有15HP，选择移动；B选择攻击。攻击先结算，A死亡，移动取消。
bash tests/scripts/scenarios.sh tests/scenarios/attack_before_move.json --out build/checks/death

# 相同低血量局面交给本版本策略，防御后剩5HP。
bash tests/scripts/scenarios.sh tests/scenarios/defend_survives.json --out build/checks/guard

# 同一局面复制成有公告解释/无解释两支，任务计分资格保持相同。
bash tests/scripts/scenarios.sh tests/scenarios/announcement_pair.json --out build/checks/blast

# 先执行0、1、2，再从Tick3复制完整暖状态，分别跑到8。
bash tests/scripts/scenarios.sh tests/scenarios/warm_forks.json --out build/checks/warm
```

需要已有`g++`和`nlohmann-json3-dev`。首次编译，之后按源码、头文件、编译器和选项缓存。非标准测试环境可加`--json-include /包含nlohmann目录的路径`，普通Ubuntu不用填。不要求手工输入环境变量。

退出码：0为全部通过；1为断言失败，仍保存现场；2为输入、构建或恢复错误。

## 起止时间

所有时间是绝对Tick，快照固定在`before_tick`：该Tick的动作尚未执行。`initial.tick=200`、`until_tick=205`执行200、201、202、203、204，保存执行205之前的状态。事件期限用`[start,end)`。

若本场Tick为500ms，Tick200对应开场后100秒；秒数按本场Tick长度转换。工具不实际等待这些秒数，也不在同一Tick的攻击与移动之间暂停。该先后关系用一个Tick的反例检查。

## 摆车辆、资源与事件

复制现有JSON后，主要修改下表：

| 字段 | 用途 |
| --- | --- |
| `name / stage / tick_limit` | 场景标识、阶段、最终状态Tick |
| `map` | 宽高、视野、基础墙、四个出生点；当前实体图支持视野半径0–4 |
| `initial.tick` | 从哪一个动作Tick前开始 |
| `initial.resources` | 此刻剩余资源，不是开局资源清单 |
| `initial.cars` | 启用的车；每辆明确给`position`、`hp` |
| `agents` | 与启用车辆一一对应的策略、NPC或脚本 |
| `missions` | 公开任务；已完成ID填在车辆`completed`中 |
| `events` | 事件物理安排、公告时间、解释交付时间 |
| `qualified` | 模拟计分资格，与是否提供解释独立；不冒充真实调用成功 |
| `run` | 前缀、终点、断点、保存点、断言和分支 |

车辆字段：

| 类别 | 字段 |
| --- | --- |
| 位置/生命 | `position / hp / alive`；死车明确给hp=0、alive=false和`respawn_in` |
| 成绩/任务 | `R / D / normal / high / completed`，最后一项是已完成任务ID数组 |
| 保护/复活 | `protected / protected_left / respawn_in`，布尔保护与剩余时间必须一致 |
| 已领事件 | `reversed / vulnerable / empowered`是事件ID数组；`capture`是ID到驻留进度的映射 |
| 治疗 | `heal_until`绝对截止Tick，`heal_amount`每Tick回血量 |
| 回执 | `last_action`属于上一Tick；`position_reason`可明确为initial/respawn等 |
| 计数 | `deaths / blocked / invalid / attacks / defends / moves` |

未指定的成绩/计数/驻留初值0，效果集合和完成任务为空，保护关闭，治疗截止-1、治疗量0、回执null，alive=true。非零Tick未明确指定的位置原因留空。这些是人工夹具的初值，不是正式机器人缺配置时的兜底。字段拼错、存活/血量矛盾、存活车辆重叠、非整数Tick、越界、未知效果ID都会报错。

省略的席位在该夹具中停用，不宣称是正式四车比赛的复活状态。完整比赛测试应显式启用四台车。

三种agent：`strategy`使用本次构建的`competition()`；`npc`用`style`选六种已有风格；`script`用`actions`按绝对Tick给动作，并必须明确写`otherwise`。第二阶段常用`{"action":"defend"}`；null是明确不提交，不默认为WAIT。

每个agent必须写`event_delivery`为`oracle`或`none`。前者在公告且到交付时刻后提供正确结构，后者不交付解释；都不改变`qualified`。

```json
{
  "id": "blast",
  "notice_tick": 200,
  "delivery_tick": 201,
  "event": {
    "kind": "explosion",
    "start": 202,
    "end": 203,
    "area": [4, 4, 8, 4],
    "amount": 35,
    "source": "测试公告：Tick202矩形[4,4]至[8,4]爆炸，伤害35。"
  }
}
```

200公告，201才交解释，202爆炸。保存断点不会重置交付时刻。`event`复用源码中的八类事件契约，`id`用于关联已领取效果和驻留进度。

世界与策略记忆要区分：直接把世界中的A设成已获无双，不代表新建策略知道过去的驻留历史。要测“自己已经知道获增益”的策略，先跑一段足够重建该效果的公开前缀。前缀可以从180开始，到200只跑20回合，不必从0开始。裁判真值不会直接注入策略。

## 按条件停止和检查

`break_when`任意一条成立，就在下一动作前停止。`always`检查起点与后续每个结算边界，`expect`只检查终点。

```json
{
  "until_tick": 215,
  "save_at": [202, 205],
  "break_when": [
    {"path": "/world/cars/A/alive", "op": "eq", "value": false}
  ],
  "always": [
    {"path": "/world/cars/A/invalid", "op": "eq", "value": 0}
  ],
  "expect": [
    {"path": "/world/cars/A/R", "op": "ge", "value": 20}
  ]
}
```

`path`是JSON Pointer，可指向血量、位置、成绩、任务集合、`/decisions/A/reason`或`/decisions/A/chosen/action`。支持eq/ne/lt/le/gt/ge/exists；路径不存在时，除exists外直接报错，防止错拼造成假通过。

## 保存、恢复、导出改摆

每次保存`前缀.json`和`前缀.分支名.checkpoint.json`；`save_at`额外保存`前缀.分支名.tick-N.checkpoint.json`。报告含逐Tick动作、原因与结算后世界，断言失败也保留最后现场。

```bash
# 从Tick5恢复，继续执行5、6、7。原终点断言不会自动挪到新终点。
bash tests/scripts/scenarios.sh --resume build/checks/warm.left.tick-5.checkpoint.json \
  --until-tick 8 --clear-breakpoints --out build/checks/continued

# 世界导出成可修改的冷场景，然后编辑坐标、血量、资源、事件。
bash tests/scripts/scenarios.sh --export-scene build/checks/blast.notice.tick-201.checkpoint.json \
  --until-tick 210 --clear-breakpoints --out build/checks/editable.scene.json
bash tests/scripts/scenarios.sh build/checks/editable.scene.json --out build/checks/edited
```

`--resume`保留世界，用保存的本队公开输入、已交付解释和动作重建策略/NPC，验证历史动作一致。拒绝误改的回放包、不同构建或历史动作分歧。墙钟搜索会受负载影响，文件重放不宣称跨机器逐位序列化全部私有状态。

`--export-scene`明确清掉策略记忆，世界另存为新夹具，允许自由修改；不能把修改血量/位置后的测试叫原历史的精确恢复。

`warm_until`与`forks`在同一进程内复制整个Game、Strategy、NPC及其随机流，前缀只跑一次。分支只能改未来脚本动作、后续解释交付方式，不允许改写旧动作，也不抹掉已理解的事件。参见两个分支示例。

这不是ROS Session或HTTP套接字的进程快照。异步解释在此用`delivery_tick`模拟，真实网络/旧session/重发/QoS仍由已有通信测试验证；自建裁判与原版裁判的一致性也需另验收。

## 小局面的最优参照

```bash
bash tests/scripts/scenarios.sh tests/oracles/early_escape.json --oracle --out build/checks/escape-best
bash tests/scripts/scenarios.sh tests/oracles/resource_chain.json --oracle --out build/checks/resource-best
python3 tests/oracle_test.py
```

`--oracle`只供测试端。它使用完整摆局、固定脚本/NPC与随机种子，枚举我方每个合法动作序列，比较1–4Tick后的明确目标；不输入参赛程序。其余车不能用耗时搜索Strategy或外部worker，因为那样不能把转移当成确定函数。

```json
{
  "seat":"A", "depth":3, "max_nodes":10000, "include_no_submission":false,
  "objective":[
    {"path":"/world/cars/A/deaths","sense":"min"},
    {"path":"/world/cars/A/hp","sense":"max"}
  ]
}
```

这是场景顶层`oracle`字段。目标按列出的优先级作字典序比较；没有暗中加血量/死亡权重。也可只最大化D，故允许明确的以命换分目标。include_no_submission必须明写：false只搜本程序的移动/攻击/防御候选，true额外纳入明示的不提交（裁判等待），便于核对完整协议动作集；不是默认WAIT。此模式的终点由depth定义，普通run的until_tick/条件断点/断言不参与这个优化；普通场景仍由run控制。

输出`前缀.oracle.json`，含各首动作的最好后续、目标值、完整终点、节点/叶子数。只有`complete=true`且`optimal_for_stated_fixture=true`才表示在该固定对手/种子/短期限/目标及声明的候选动作集下搜完所有序列；不纳入等待时不能声称协议全部动作最优。预算耗尽退出1，没到完整叶子时best_sequence为null。新叶子不覆盖初始世界。

early_escape示例能区分“200先移动存活”与“先打再在201移已来不及”；resource_chain显示两步拿30优于先取右侧10。它是小样例最优参照，不是未知地图、未知对手、500Tick整局最优证明，也不能把裁判未来安排带进参赛输入。

## 文件与回归

| 文件 | 主线职责 |
| --- | --- |
| `tests/arena_checkpoint.cpp` | `Game::checkpoint/restore`保存、校验完整世界；先恢复副本，失败不污染原世界 |
| `tests/scenario_runner.cpp` | `make_scene`读地图/事件；`Agent::act`只接本队输入；`Controller`管前缀/恢复/复制；`run`执行到条件并断言 |
| `tests/run_scenarios.py` | `build`缓存源码指纹；`read_bundle/save_bundle`检查误改；main处理运行、导出与结果文件 |
| `tests/scenario_test.py` | 时序、随机流、状态恢复、迟到解释、输入隔离和故障产物回归 |
| `tests/scenario_runner.cpp::oracle` | 在明示模型/期限/目标/候选集上穷举；完整与截断结果分开 |
| `tests/oracle_test.py / tests/oracles/*.json` | 穷举完整性、权限、目标取舍与两个小决策题 |
| `tests/scenarios/*.json` | 13份JSON/15条run；包含可复制的起步例子、公告与DeepSeek现场 |

```bash
python3 tests/scenario_test.py
python3 tests/scenario_test.py --sanitize
```

普通改图不必重新开启sanitizer或重编译。代码/配置变更后才产生新的构建指纹与缓存程序。

DeepSeek新增的`highd_death_penalty / death_floor_threshold / mutual_kill_order / lowhp_retreat_survives / lowhp_retreat_late`已保留。它们检查高D扣60与封底、互杀入账顺序和200/201撤离差异；连同旧探针共13份JSON场景，由`scenario_test.py`逐份运行。
