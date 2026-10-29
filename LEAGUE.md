# 任意局面与历史版本对打

用 [SCENARIOS.md](SCENARIOS.md) 摆好时间、车辆、资源和事件，再把席位换成已冻结版本。每辆车每场一个独立程序，保留自己的跨Tick记忆；只接自己的公开感知与已交付解释。

这层是快速离线裁判，不启动ROS或真实模型，不修改`practice/`。手写NPC不是原版`robot_node`，原版对局仍需另测。

## 三步运行

需要g++、Python 3、nlohmann-json3-dev。在正常Git克隆中：

```bash
git fetch origin
# 从Git对象读完整提交，不改工作区，不混入未提交改动。
bash league.sh freeze old --ref 65c87145ae3b15572c3b1674181a74677fe86770
bash league.sh freeze current --ref 8da0edecf59218647c83a19536da1c822cbcfd0d
bash league.sh suite \
  --versions old=build/league/versions/old current=build/league/versions/current \
  --anchor old --out build/league/plan.json
bash league.sh run build/league/plan.json --limit 2 --out build/league/smoke
# 两个候选×144局面=288组合；去掉limit跑全表。
bash league.sh run build/league/plan.json --out build/league/full
```

`--anchor`固定对手版本；比较候选时对手池不变。归档组让候选和三辆anchor同场。一个旧版本是一套旧源码及旧参数，不是新代码改几个权重。

只查单格桥：`bash league.sh run build/league/plan.json --cases 'bridge-*-pressure' --out build/league/bridge`。同一命令重跑复用相同签名的已通过局面，中断局面重跑。计划/版本/裁判构建变了就换输出目录，工具会拒绝混批。

## 直接摆任意初态、定观察范围

`cases[].scene`沿用短场景格式：

| 要改什么 | 字段 |
| --- | --- |
| 当前Tick | `initial.tick`，例如200 |
| 各车位置/HP/保护/复活 | `initial.cars.A/B/C/D` |
| 成绩/任务/已领效果/驻留 | 车辆的`R/D/completed/empowered/capture/...`，完整字段见SCENARIOS |
| 此刻剩余资源 | `initial.resources`，不是开局资源表 |
| 公告/解释/生效/结束 | `notice_tick/delivery_tick/event.start/event.end` |
| 观察到某Tick | `run.until_tick` |
| 观察到死亡/得分等条件 | `run.break_when` |
| 全程/终点断言 | `run.always / run.expect` |
| 中途保存世界 | `run.save_at`，完整Tick边界的世界JSON |

用历史版本与低血仍攻击的对手：

```json
{
  "A":{"type":"external","version":"old","event_delivery":"oracle"},
  "B":{"type":"npc","style":"berserker","event_delivery":"none"}
}
```

`$focal`替换为当前候选，`$anchor`替换为固定对手，也可直接写清单中版本名。其余席位可用`npc/script/strategy`；真正历史版本必须用`external`。每个启用席位都要有明确agent。没有默认WAIT替换。

```json
{
  "until_tick":205,
  "save_at":[202],
  "break_when":[{"path":"/world/cars/A/alive","op":"eq","value":false}],
  "always":[{"path":"/world/cars/A/invalid","op":"eq","value":0}],
  "expect":[{"path":"/world/cars/A/R","op":"ge","value":20}]
}
```

200起到205止执行200–204；条件在完整Tick边界检查，不在攻击和移动之间增加现实中不存在的行动机会。条件用JSON Pointer和eq/ne/lt/le/gt/ge/exists；拼错路径会报错。

直接摆局是**策略冷启动**。世界已有无双，不等于新策略知道过去如何领取；测知情状态要从稍早时刻跑一小段本队公开输入前缀。独立历史进程的私有对象没有序列化接口，save_at只保存世界，tape保存输入/动作；不能称热进程快照。内存暖分叉与校验重放继续用`scenarios.sh`。联赛遇到`forks/warm_until`会明确拒绝，请写两份配对case，不会忽略这些字段。

## 12类局面与对手

| 类型 | 要暴露的问题 |
| --- | --- |
| sparse | 开阔稀疏基线 |
| rooms | 门口绕路、争格 |
| bridge | 唯一单格桥、执拗争路 |
| snake | 曼哈顿近但实际路径长 |
| ring | 中央封闭块外环 |
| dead_ends | 多条单格盲支路 |
| wide_lanes | 宽通道中的替代路线 |
| asymmetric | 不对称障碍、局部死路 |
| center_race | 中央出生和资源争抢 |
| resource_bias | 资源偏在一侧 |
| dynamic_cut | 爆炸与分裂相邻、桥动态切断 |
| late_empty | 从200起无资源，继续到500 |

这是12个地图/资源/时间组合，不是12种不同几何。除晚期空图外资源200，4高+12普通；24×24、视野4。保存墙数、可达格、死路、窄格和出生点路径距离，不强行开四条外围高速路。目前尚无旋转/镜像、全部尺寸分层和原版配置校验器验收，不称覆盖完整。

经济组：collector/opportunist/defender。压力组：berserker低血仍攻，sentry原地占路，trickster有射击机会时逐Tick交替攻防。归档组：三辆anchor。fortress/aggressive/wanderer仍可手工加入。新增行为打破共同的“低血必防”假设；不宣称九种名字就是九套完整独立算法。NPC也只看自身感知。

## 冻结与结果

`versions/名字/manifest.json`包含完整提交SHA、策略/战术/事件及所有头文件SHA256、适配器SHA、编译器/选项、JSON头文件SHA、程序SHA256和**全部StrategyParameters实际值**。不导出密钥/令牌。离线worker读感知席位，同一冻结程序可用在四席位；ROS编译期席位是另一层。

同名版本不覆盖，失败构建有failed.json，不能冒充成功。每次调用验证清单、源码、适配器和程序未改。很旧的提交若不兼容Strategy接口，明确编译失败，需单独适配，不换成新策略。只有远端源码快照时可用`freeze 名称 --snapshot 目录 --lock 清单.json`，清单含repository/完整commit/git_blobs且逐文件校验；它应来自可信提交树，不是签名认证。普通克隆用--ref即可。

| 结果文件 | 用途 |
| --- | --- |
| report.md | 版本、总体、配对差值和每局全表，失败/未跑不算胜负 |
| summary.json | 机器可读结果 |
| plan/identity/engine/versions.json | 原计划、批次签名、裁判构建、版本及完整参数 |
| 局面/版本/result.json | 原始R/D/M/名次、死亡/无效/受阻、世界、时延、交付/授予观察 |
| tape.jsonl.gz | 本队公开输入、解释、动作、原因和耗时；gzip逐行查看 |
| *.stderr.log | 崩溃/超时错误，不伪造动作 |
| tick-N.world.json | 某Tick的完整世界，不含私有进程状态 |
| failure.cold.scene.json | 失败终点可编辑冷场景，不冒称保留策略记忆 |

分数、平均名次、第一份额一起看。配对按同case，先按原始地图聚合再自助采样；四席位不算四张独立图。小样本或反复调同族不能证明泛化。所有断言失败也列在逐局表里，不能只报胜局。分析报告和完整实验归档放研究仓库。

徘徊是连续12帧在至多4格、R/D/任务/死亡无进展的候选窗口；合理守点也会触发，需看tape。时延记p95/max和超过500ms次数；进程硬超时默认5秒是故障检测，不是动作截止仿真。

公告unique_delivered/gained_fortress/gained_heal只证明送达/授予。因果收益需同资格、同局面/对手/种子，配对及时、迟到、无解释和同信息关单项机制，记录少受伤/有效输出及绕路代价。真实HTTP、格式/语义错误、更新公告与实际模型回放仍需另验。

## 文件主线与回归

| 文件/入口 | 职责 |
| --- | --- |
| league.sh | 转到根目录，进入统一CLI |
| version_store.py | Git取固定提交，生成参数导出，冻结并验哈希 |
| version_worker.cpp | 初始化旧Strategy；逐行接本队输入，返回动作/原因/耗时 |
| scenario_runner.cpp --serve | 裁判按步结算，全图与各车输入分开 |
| league_maps.py | 生成结构地图、指标与144case清单 |
| version_league.py | Lines管进程，play跑局面，summarize做配对，markdown列全表 |
| version_league_test.py | 冻结误改、脏工作区、输入隔离、条件停止、独立进程、缓存、故障产物 |

```bash
python3 tests/version_league_test.py --version build/league/versions/old
python3 tests/scenario_test.py
```

非标准环境可在freeze/run/回归加`--json-include /包含nlohmann的目录`。正常Ubuntu安装依赖后，不要求环境变量。
