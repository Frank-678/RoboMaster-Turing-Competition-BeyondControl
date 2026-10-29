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

## 原版ROS与真实模型联赛

快速离线用`league.sh`；原版`practice/`、DDS、真实模型用`practice_league.sh`。两个入口保留，不能把离线胜率当作原版实测结果。同名冲突已处理：DeepSeek原`tests/version_league.py`改名为`tests/practice_version_league.py`，离线文件保留原名。

先在唯一的`match_config.hpp`填好`practice_llm_url / practice_llm_key / practice_llm_proxy / practice_port / ai_model`和ROS域，再`bash build.sh`。`practice_batch.sh`通过C++启动器把这些已编译值交给原版裁判；不另读`llm.env`，不需要手工export。真实调用会消耗配额。

```bash
# 先核对清单里已有的冠军/旧冠军路径、二进制摘要；不会重新命名已有版本。
bash practice_league.sh --maps 20261201,20261202 --mixes M1,M2 --out build/practice-league/pilot
# 同图同事件只转四次出生点；也能单独跑批量驱动。
bash practice_batch.sh --seed 20261201 --map-locked --count 4 --summary build/practice-league/example.json
# 补编号3，必须与原批count/seed/summary和程序身份一致；完整生成再筛选。
bash practice_batch.sh --seed 20261201 --map-locked --count 4 --only 3 --summary build/practice-league/example.json
python3 tools/archive_batch.py --summary build/practice-league/example.json --to build/practice-league/archive
python3 tests/practice_tools_test.py
```

每个summary旁有独立`.runs/case-编号-随机后缀/`，内含本局输入与机器人/裁判日志。summary记录配置摘要、各席实际二进制摘要和原版session；归档从每条记录自己的目录取日志，包括失败局。旧摘要若没有这些身份字段，另用新输出路径，不悄悄覆盖。完成局同身份重跑会复用，失败局重跑保留此前日志目录。筛选后的完成数只指本次请求的编号。

`--base-config PATH`可换完整练习模板，需保持18×18及四席结构；当前实测生成器仍只是六墙随机扰动，不能称为12种地形全覆盖。离线12局面族已经提供单桥、房间、蛇形、死路、动态封路和空资源末局等；把这些有结构的模板迁入原版配置并验证，是下一项真实联赛工作。M3未配候选就列为跳过，实际完成数不凑144。

`tools/versions.json`保留DeepSeek的champion=`eb321df`、old_champion=`9bc72a7`及原二进制/钉集；这和本轮离线比较的65c8714/8da0ede是两套标识，不互换。机器上若找不到冻结程序，先核对本地路径；不能拿最新bot覆盖旧冠军再沿用旧名。

### 实测工具的文件与函数

| 文件/函数 | 做什么 |
| --- | --- |
| `practice_league.sh / tests/practice_version_league.py` | 按图seed、对手混合、四次出生轮换调度原版对局，核对冻结程序；缺局和0/0都失败 |
| `practice_batch.sh / practice_launcher --batch` | 唯一配置入口，把模型、密钥、出网代理、ROS域和监听端口交给驱动，不打印密钥 |
| `tools/batch_practice.py::variants` | 固定随机流先生成各编号，再筛选补跑，避免重测另一张图 |
| `config_digest / seat_hashes` | 记录完整地图配置与实际各席程序的身份，拒绝混用旧摘要 |
| `owned_run / run_battle` | 从本机API确认session、目录和配置；等待四席注册、启动、观察结束，所有异常都回收 |
| `stop_battle` | 只停止已确认session并清理本次进程组；组长已退出仍收子进程 |
| `tools/build_seat_variants.sh / .py::snapshot` | 在独立源码副本中修改team与公开practice令牌，构建各席并记录实际配置/源码/二进制摘要 |
| `tools/run_seat.sh` | 启动指定安装底座的bot；节点名重映射不等于改变已编译席位 |
| `tools/archive_batch.py::scan_keys / main` | 扫描目录和摘要单文件，按session归档各局日志、输入、分数及失败状态，写可读战报 |

主数据类型：`variant`是完整裁判配置字典；`our_seats`是席位→安装目录；`identities`是席位→程序摘要；`results`是编号→状态记录；`session`是裁判场次标识；`processes`只包含本次独立进程组，不是机器全部进程。
