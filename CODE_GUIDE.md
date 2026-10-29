# 源码导读

历史版本联赛的冻结、逐帧输入与全部参数清单见[LEAGUE.md](LEAGUE.md)；原版ROS入口是`practice_league.sh`，实测工具函数见该文末。

短场景工具的文件、函数、状态字段与起止Tick语义见[SCENARIOS.md](SCENARIOS.md)。它是独立测试入口，不被正式ROS机器人链接。

先读比赛配置，再沿“收感知→管场次→作决策→发动作”阅读。变量的具体含义写在声明旁，关键规则写在判断旁。

[规则状态机](STATE_MACHINE.md)将条款、权限状态、事件和对手轨迹转移对应到代码；[实测记录](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/REAL_TEST.md)区分真实模型与完整裁判对局。

## 主线从哪里开始

1. `bot.cpp::main`把头文件中的ROS网络设置交给DDS，初始化ROS，创建`Bot`。
2. `Bot`注册队名、订阅本队感知。每收到一帧，就调用`on_perception`。
3. `Session::act`判断这是哪一场、哪一个Tick。换场清旧状态，同Tick复用缓存，旧Tick丢弃。
4. 有AI结果就校验并合并事件；有新公告就排后台请求。这里不等网络。
5. `Strategy::decide`读真实坐标、更新地图、重建效果、判断危险，再比较资源/任务/攻击，选一个动作。
6. `action_for`补上当前Tick和场次，检查动作与阶段。第二阶段存活时兜底DEFEND；不能合法防御时不发布。
7. `Bot`发布动作。下一帧以`self.position`确认实际位置，用`last_action`解释上一步有没有执行成功。

另一条线程走`AiWorker::run → perform → ready_`。主线程下一Tick通过`poll`取结果，不会把HTTP等待塞进感知回调。

## 每个文件做什么

表中跨仓链接为研究记录，已从代码仓库移出。历史JSON报告归分析仓库；仍在使用的selected-parameters.json保留。

| 文件 | 职责 |
| --- | --- |
| `README.md` | 先看这里：地址、令牌、模型分别填哪，两个终端如何启动，零次调用如何查 |
| [LESSONS.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/LESSONS.md) | 七份字幕的借鉴/否决、假设风险逃路修复与验证边界 |
| `tests/risk_benchmark.cpp`、`tests/run_risk.py` | 四席位×四旋转×两敌方动作的最小反例与可选旧工程配对 |
| [NAVIGATION.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/NAVIGATION.md) | 无收益往返、争格、封闭区域的复现、脱困机制和日志说明 |
| `tests/unstuck_benchmark.cpp` | 六个500Tick导航压力/正常路径夹具，与旧核心配对 |
| `tests/run_unstuck.py` | 编译导航夹具，断言脱困与保留正常绕路，可只读比较旧工程 |
| `ARENA.md` | 自建对手池、六类地图、规则模型、参数搜索和留出边界 |
| `arena.sh` | 一键离线对战与调参；不改正式参数 |
| `DEV.md` | 依赖、策略机制、事件表、测试方法和当前验证范围 |
| `CODE_GUIDE.md` | 本文件：主线、文件、类型和函数索引 |
| `STATE_MACHINE.md` | 全部编号规则对应、状态图、策略路径与预测边界 |
| [REAL_TEST.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/REAL_TEST.md) | 真实模型调用及裁判启动结果 |
| [MATERIALS_CHECK.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/MATERIALS_CHECK.md) | 原始材料和已有学习笔记具体对应哪些修复，以及验证范围 |
| `LICENSE` | 本工程许可证 |
| `.gitignore` | 排除构建、安装和日志等产物 |
| `build.sh` | 在正确工作空间构建选手包，生成编辑器编译数据库 |
| `run.sh` | 加载ROS环境，启动已构建的机器人；配置不从命令行或环境补值 |
| `practice.sh` | 启动我们自己的练习配置入口，原版`practice/`不变 |
| `check_ai.sh` | 验证诊断与启动器构建摘要，再选择一次上游检查；不加载ROS或启动裁判 |
| `scripts/check_ai.py` | 最小真实上游请求，区分代理/TLS/连接/读取/HTTP/模型外壳失败，不打印密钥或响应正文 |
| `src/rmtc_bot/package.xml` | ROS包名、版本、构建和运行依赖 |
| `src/rmtc_bot/CMakeLists.txt` | 构建核心库、ROS入口、练习启动器和核心测试，安装可执行文件 |
| `include/rmtc_bot/match_config.hpp` | 唯一赛前配置：身份、阶段、ROS、两段AI链路、权重和阈值 |
| `include/rmtc_bot/strategy_tuning.hpp` | 旧include路径的兼容入口，只包含`match_config.hpp`，没有第二份参数 |
| `include/rmtc_bot/strategy.hpp` | 策略接口、地图格/历史快照和跨Tick记忆 |
| `src/strategy.cpp` | 实际决策：地图、路径、任务、攻防、八类事件和未知格试探 |
| `include/rmtc_bot/strategy_parameters.hpp` | 参数值对象；正式从编译配置构造，只有离线程序可注入不同取舍 |
| `include/rmtc_bot/rules.hpp` | 规则常量、同时战斗、移动和复活落点；不参与参数搜索 |
| `include/rmtc_bot/tactics.hpp`、`src/tactics.cpp` | 可选三Tick局部对手情景搜索，返回首动作 |
| `include/rmtc_bot/events.hpp` | Tick/Cell、八种事件、模型提案和校验接口 |
| `src/events.cpp` | 校验模型JSON、生成稳定事件键、构造公告解释Prompt |
| `include/rmtc_bot/scoring.hpp` | 官方封顶计分前后的预计分差 |
| `include/rmtc_bot/protocol.hpp` | 检查感知身份，构造合法动作，处理阶段/保护/死亡限制 |
| `include/rmtc_bot/session.hpp` | 一场比赛的主流程接口和缓存状态 |
| `src/session.cpp` | 换场、Tick去重、AI结果采纳、决策与动作缓存 |
| `include/rmtc_bot/ai_worker.hpp` | HTTP配置、任务/结果类型、线程状态和调用诊断 |
| `src/ai_worker.cpp` | curl请求、公告去重、后台线程、取消、退避、mock识别和调用统计 |
| `src/bot.cpp` | ROS发布订阅、队名注册、日志与进程入口 |
| `src/practice_launcher.cpp` | 把统一配置交给原版练习程序，再执行它；不改它的文件 |
| `tests/strategy_test.cpp` | 基础感知生成、地图/边界/坏输入回归和合成迷宫 |
| `tests/full_strategy_test.cpp` | 任务、攻防、事件、场次、DEFEND/试探和HTTP场景 |
| `tests/mock_gateway.py` | 本机固定HTTP响应，验证真实客户端的协议/错误处理；没有真实模型 |
| `tests/practice_gateway.py` | 只读加载原版HTTP处理器，验证模型名优先级、令牌替换、代理和A席位记录；上游仍是夹具 |
| `tests/startup_test.py` | 在临时工作空间检查旧配置启动拦截、固定二进制路径和练习启动环境 |
| `tests/ai_check_test.py` | 本机HTTP及注入异常检查诊断分类、正文读取超时和敏感文本不回显 |
| `tests/arena.hpp` | 测试裁判与陪练的数据类型和接口，不被参赛程序包含 |
| `tests/arena.cpp` | 局部观测、整Tick结算、八事件、任务与结果输出 |
| `tests/arena_scenarios.cpp` | 六类地图及不同种子的公开任务/自然语言公告场景 |
| `tests/arena_opponents.cpp` | 六种只用本队公开观测的手写陪练 |
| `tests/arena_main.cpp` | 仅离线JSON入口，组装新鲜策略、裁判和逐局指标 |
| `tests/arena_test.cpp` | 人工可算规则边界与输入隔离检查 |
| `tests/arena_settings.json` | 冻结对照参数、训练/留出种子和风格列表 |
| `tests/run_arena.py` | 编译、逐轮训练筛选、留出与模块消融；保存结果 |
| `tests/results/selected-parameters.json` | 训练选中候选，不自动生效、不由机器人读取 |
| `tests/run.sh` | 用g++构建离线测试，启动HTTP夹具，退出时清理临时文件 |

表中的`include/`和`src/`短路径均相对`src/rmtc_bot/`。

## 命名空间、类型和符号

| 名字 | 含义 |
| --- | --- |
| `std` | C++标准库命名空间；例如字符串、容器、线程和锁 |
| `nlohmann::json` / `Json` | JSON值类型与它的短别名，可表示对象、数组、字符串、数字或null |
| `rclcpp` | ROS 2 C++节点、发布订阅、QoS和执行器接口 |
| `std_msgs::msg::String` / `Message` | ROS消息外壳，`data`里装JSON文本 |
| `rmtc` | 我们自己的公共类型和模块命名空间 |
| `rmtc::match_config` / `cfg` | 唯一编译期比赛配置及短别名 |
| 匿名`namespace {}` | 只供当前.cpp使用的小工具，不暴露为公共接口 |
| `Tick` | 64位逻辑回合，不是毫秒或收到消息的时刻 |
| `Cell` | 整数对：`first=x`向右，`second=y`向下 |
| `EventKind` / `Event` | 事件类型枚举与已经校验过的参数对象 |
| `EventProposal` | 模型解释的已知事件`events`和未知说明`unknown` |
| `Tile` / `Snapshot` | 地图格记忆、某帧本车位置及上一Tick执行反馈 |
| `TurnMode` | dead/collecting/guarded/combat_ready四种动作权限状态 |
| `OpponentTrack` | 公开敌车ID对应的最近位置、位移、Tick和连续一致次数，换场清空 |
| `Enemy` / `Entity` / `Node` | 当前可见敌车、解析后实体、BFS的第一步与距离 |
| `AiConfig` / `Job` / `Result` | 固定HTTP设置、待执行请求、带场次来源的结果 |
| `CurlRuntime` / `Download` / `Cancellation` | curl全局资源、正文缓冲、换场取消上下文 |
| `optional<T>` | 有一个T或为空；空动作表示不发布，空AI结果表示尚未完成 |
| `array<T,N>` / `vector<T>` | 固定长度数组、动态列表；四向/五动作数组与索引顺序固定 |
| `map<K,V>` / `set<T>` / `queue<T>` | 键值记忆、去重集合、BFS先进先出队列 |
| `unique_ptr` / `SharedPtr` | 独占资源、共享持有ROS对象；离开生命周期自动释放 |
| `mutex` / `lock_guard` / `unique_lock` | 共享状态锁、作用域锁、可配合条件变量等待的锁 |
| `atomic` / `condition_variable` / `thread` | 无需普通锁读取的代数、唤醒后台任务、后台执行线程 |
| `steady_clock` | 只用于网络退避和耗时；不会随系统校时倒退，也不代替比赛Tick |
| `const` / `&` / `*` / `_`后缀 | 不修改该值、引用已有对象、指向对象、类成员变量的命名约定 |

## 函数索引

| 模块 | 函数及其工作 |
| --- | --- |
| `bot.cpp` | `main`设置网络并运行ROS；`Bot`构造发布/订阅和Session；`on_perception`解析、处理、发布与记日志 |
| `session.cpp` | 构造函数固定阶段/AI设置；`act`执行整帧主线；头文件的`reason/ai_status/ai_http_success/ai_diagnostics`只读取日志快照 |
| 协议 | `frame_tick`验证信封；`turn_mode`建立本帧动作权限，`turn_mode_name`供日志；`action_for`按权限过滤并回填身份 |
| 坐标工具 | `integer`检查整数；`absolute_position`读[x,y]；`relative_position`读实体图；`manhattan`算四向距离；`ray`判断普通攻击方向 |
| 策略状态 | 构造函数固定阶段；`reset`换场清记忆；`merge_events`合并解释；`known_events`生成LLM上下文；`reason`返回决策原因 |
| 对手预测 | `predict_opponent`由同ID连续位移推测一格，断帧/瞬移重置；`decide`检查预测合法性，只软调整移动收益 |
| 策略时序 | `split_wall`判断临时墙并豁免已知出生点；`dangerous`判断已知环境危险；`safe_first_steps`倒推8Tick可接续路线；`update_effects`从实际回执重建效果；`decide`选本Tick动作 |
| `decide`局部函数 | `fallback`合法防御或不发布；`skip`明确不发布；`move`补偿幻术；`possible_fire`估计可见敌人踩辣椒风险；`combat_value`估计新增D价值；`consider`更新最高收益候选 |
| 计分 | `score_delta`用当前累计值和预计变化计算封顶前后分差，不修改官方成绩 |
| 事件 | `active`检查[start,end)；`contains`检查范围；`event_summary`生成摘要；`decode_event_proposal`校验完整提案；`decode_events`是只取events的测试便捷接口；`event_system_prompt`返回解释公告的提示词 |
| 事件私有工具 | `integer`检查有界整数；`number`检查有界有限实数；`model_object`提取模型输出中的单一完整JSON并去掉字符串外尾逗号；都不解释公告语言 |
| AI入口 | 构造函数检查配置并启线程；析构函数取消并join；`reset`换场；`observe`收公告并排请求；`poll`取结果；`acknowledge`接收校验反馈并决定退避/停用 |
| AI后台 | `perform`发一次HTTP；`run`取任务、释放锁、联网、放结果；`status/enabled/observed_http_success/diagnostics`只读取工作状态 |
| curl回调 | `CurlRuntime`构造/析构初始化/清理；`Download::append`收正文并限长；`Cancellation::progress`检查代数，换场取消 |
| 练习启动器 | `main`检查固定配置、设置原程序需要的环境并exec原脚本；`set`局部函数只设置一个明确变量 |
| 基础测试 | `frame/entity`生成公开消息；`check/expect`断言；`scenarios`测基础分支；`collect_in_synthetic_maze`模拟已知规则的采集；`main`汇总测试 |
| 高阶测试夹具 | `fresh/robot/specification`生成感知/敌车/事件；`choose`运行一帧；`events/rejected`验证正式解析器 |
| 高阶测试场景 | `combat_and_missions/capped_score_decisions/defend_fallback/event_validation/hazard_and_illusion/future_escape_route/fortress_and_effects/session_contract`分别验证其命名机制；`full_scenarios`汇总；`gateway_scenarios`测本机HTTP |
| 状态/轨迹测试 | `turn_state_transitions`验证动作权限转移；`opponent_prediction`验证争格/未来射线及轨迹失效退路 |
| Python夹具 | `Handler.log_message`静默访问日志；`do_POST`校验请求并返回指定夹具；`main`开服务、运行测试、核对次数并清理 |
| 原版代理测试 | `original_handler`核对原件并只读导入；`ModelProxy.do_POST`检查实际转发的模型名/头/选项；`main`启动临时HTTP链路并核对原版A席位计数；`practice_gateway_scenario`验证正式Session采用结果 |
| 启动测试 | `run_guard`只在临时副本替代ROS加载；`main`检查配置摘要，再编译真实启动器并用exec占位断言环境/参数；不启动裁判 |

函数参数和成员含义以声明旁的中文注释为准。循环里的`x/y`是坐标，`dx/dy`是偏移；`key/e`是事件键和内容；`cell/tile`是格子与记忆；`state_tick/state`是感知Tick与历史快照；不同作用域的同名变量互不共享。

## 原版practice：只说明，不修改

| 原文件/目录 | 用途 |
| --- | --- |
| `practice/start_referee.sh` | 原版裁判启动脚本，接受阶段、端口、AI模式等参数 |
| `practice/practice.py` | 练习进程管理、网页服务和模型转发代理 |
| `practice/start_bot.sh` | 启动原版参考/陪练车 |
| `practice/internal_ros.py` | 练习系统内部ROS通信桥接，由原版程序使用 |
| `practice/bin/referee_node` | 裁判进程；`robot_node`是参考车；`scoring_node`计分；`simulator_node`推进模拟 |
| `practice/bin/validate_config` | 原版练习配置校验程序 |
| `practice/config/stage1.json`、`stage2.json` | 人工练习场景配置，选手决策程序不读取这些地图真值 |
| `practice/index.html` | 人工调试网页，不是机器人输入 |
| `practice/manifest.json` | 发行文件清单/校验信息 |
| `practice/README.md`、`测试使用教程.md` | 原版使用说明 |
| `practice/CONTESTANT_PROTOCOL.md`、`rules.txt` | 随包协议与规则材料；正式使用仍核对最新发布版本 |
| `practice/__pycache__/` | 原Python程序生成/附带的字节码缓存 |

这些原文件从初始构建起保持不变。我们的`practice_launcher.cpp`只传启动参数/环境，没有写入、替换或修补原版裁判。


## 新增目标与资源规划

详细行为与实际结果见[PLANNING.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/PLANNING.md)。原通信与AI结构不变。

| 类型或函数 | 用途 |
| --- | --- |
| `Goal` | `position`目标坐标、`kind`类别、`tick`选择时刻；`goal_`仅在选择MOVE时续签 |
| `ResourcePlan` | `first`世界首方向，`first_resource`首次拾取点，`utility`收益/耗时，`benefit`整串收益，`stops`未执行目标串 |
| `resource_plans` | 只在已知格比较有界资源串，不写回真实位置、地图或score |
| `resource_plans`的参数 | `start/tick`本帧真值，`remaining_ticks`剩余动作数，`targets`少量资源候选，`occupied`当前敌车占位，`first_allowed`原BFS批准的首步，`current_r`官方R，`need_normal/high`任务机会，`value_target`资源任务门槛 |
| 局部`State` | `position/elapsed`预计落点与耗时，`first/first_resource`首动作与首份资源，`virtual_r/benefit`预计资源与估值，`collected`防重复拾取，`mission_bonus`防重复支付同类任务机会 |
| `route` | 单段BFS；`parent`保存前驱和步数，`pending`队列，`path`依序经过格，`action_tick`预计执行时间 |
| `deadline/time_up` | 墙钟软预算，不改变逻辑Tick；超时保留已有基线或已完成的安全候选 |
| `beam/expanded/selected` | 当前层、扩展层和剪枝后下一层；只存在于本帧 |
| `best/used_direction/unique` | 按首方向保留资源串，给不同方向名额，以首步/落点/拾取集合/耗时去重 |
| `ranked_before` | 严格排序，先比较收益/耗时，再比整串收益、耗时、方向和位置 |
| `previous_goal/keep_goal/best_goal` | 上次目标、是否能给迟滞奖励、本帧候选；失败MOVE、断帧、复活不沿用奖励 |
| `resource_options/targets/first_allowed` | 单目标排序得到的少量目标与可用首步；不重新放开占位或危险格 |
| `best_benefit` | 同类资源单位收益近似打平时，比较实际计划兑现总量 |
| `newly_visible` | 观察点按本帧实际视野半径可能新揭开的格数，未知内容仍未知 |
| `tick_limit/remaining_ticks` | 配置给出的100/500动作窗口和本帧剩余步数，最终状态不发布动作 |

新增配置：`goal_retention_bonus`迟滞余量，`resource_plan_depth`目标深度，`resource_beam_width`束宽，`resource_candidate_limit`候选数，`resource_plan_budget_ms`额外搜索预算，`frontier_value`信息权重；均是赛前明确取舍。`stage1_ticks/stage2_ticks`是规则时长配置，不作为优化器搜索参数。

`tests/planning_benchmark.cpp`只有测试端持有完整合成地图，逐帧生成局部感知；`Fixture`保存场景，`entity`生成相对图，`run`执行独立策略并记录真实移动/拾取、覆盖格、受阻次数与决策耗时，`main`输出七例。`receipt`来自测试端已执行动作，`seen`只用于评测，不作为选手输入。

## 上游出网诊断的主线

`check_ai.sh → practice_launcher --check-ai → scripts/check_ai.py::main → probe`。仍只有`match_config.hpp`一处配置；进程环境只是启动器向Python传值，不要求用户输入。正常`practice.sh`入口不变。

`check_ai`标志决定做诊断还是启动原裁判；`argc/argv`只接受这一内部选择。`build.sh`生成`ai_check.sha256`，记录启动器源码、诊断脚本、shell入口及已构建二进制的哈希。没有新记录或任一字节变化时拒绝执行，防止旧启动器忽略新参数后误启动裁判。

Python的`probe`只发一次POST：`url/key/model/thinking/json_output/max_tokens`均由编译配置传入；`payload`是最小JSON请求，`started`是单调计时起点，`response/body/choice/content`是本地临时结果，`result`仅含错误分类、HTTP码和耗时。`ReadTimeoutError`处理requests把正文读取超时包装成连接错误的情况；其余连接错误不靠异常文本猜原因。

`main`检查启动器交接变量，调用`probe`，再输出`upstream_proxy`是否明确配置、`upstream_host`主机及模型名；没有打印Key、URL查询参数、模型思考或响应正文。退出0只代表这个最小请求返回预期JSON，不代表完整公告或比赛资格。

## 战术、规则和离线对战符号

| 类型/命名空间 | 成员与职责 |
| --- | --- |
| `StrategyParameters` | 固定的一组权重/预算；`competition`从唯一配置复制，`validate`拒绝坏值；不含可调规则伤害 |
| `rmtc::rules` | 确定规则函数的命名空间，`ActionKind/Action`表示动作及方向，none只表示不提交 |
| `Fighter` | `position/hp`位置血量；`attack/defended`对普通/防御目标伤害；`vulnerability`受击倍率；`alive/protected_spawn/area_attack`三种权限/效果 |
| `CombatResult` | `fighters`统一扣血后状态，`earned`本批普通D及击杀奖，`deaths`各车死亡次数；死亡扣除最后统一做 |
| `TacticalMission` | 类型、门槛、到点坐标；只包含未完成任务 |
| `TacticalInput` | 0号本车、1至3号可见敌车；地图/资源/事件、目标价值和目标坐标、首步安全、当前R/D、可信拾取次数及效果记录；字段旁逐项注释 |
| `TacticalChoice` | 首动作、估值、情景数与模拟步数，不暴露必须执行的后续队列 |
| 战术局部`State` | 四车副本、剩余资源、虚拟R/D、拾取次数、死亡数、任务标记/估值、堡垒进度、各车回血截止和数量 |
| 战术局部`Branch` | 一个公共动作前缀在多个情景下的状态数组、各情景折扣收益与合计估值 |
| `GoalDistance` | 战略目标、权重、起点距离和已知格反向BFS距离；末位置真实接近目标才有尾部估值 |
| `MoveAttempt` | 已提交的起点、落点、Tick；`move_attempt_`等回执，`failed_destination_/failed_moves_`只计连续失败 |
| 资源局部`State::stops` | 本分支显式资源目标；`planned_resources_`只保存最终选中方向的目标串 |
| `route_cache` | 本帧`(起点,目标,已用Tick)`到路径的缓存，不跨帧保留 |
| `direction_value/goal/benefit/plan` | 每个世界首方向的最好收益、目标、总收益和资源串，战术改选首步时保持对应 |
| `rmtc::arena` | 仅离线测试裁判；`Scenario/Announcement`持有测试真值，`Car`持有实际结算；`Game`推进，`Opponent`只用局部输入 |

| 函数/局部函数 | 直观用途 |
| --- | --- |
| `rules::decode_action/encode_action` | JSON和测试动作之间转换；不提交编码为null |
| `rules::ray/manhattan` | 判断1–2格攻击方向、四向距离 |
| `rules::combat/adjusted_damage` | 同时伤害与分奖；全部奖励后扣死亡60，最低0、不封顶累计D |
| `rules::move/respawn` | 按存活起始占位统一移动；曼哈顿层/上右下左找合法复活落点 |
| `tactical_choice` | 展开三Tick、按不同首动作保留束，输出首动作 |
| `can_move/choices` | 已知地图内生成可移动和有实际目标的攻击，以及合法防御 |
| `opponent_action` | 某个手写对手情景在该状态下的合法候选，不访问未来真值 |
| `advance` | 复制状态推进一Tick；`fire`按受害者/来源积累辣椒贡献 |
| `value/aggregate/tail_value` | 官方边际分+机会成本；情景均值/最差值；真正接近战略目标的尾部价值 |
| 资源`extend/evaluate_stops/record` | 接一个资源目标、从真实起点重评一串、保存各首方向最好串 |
| `Strategy::note_model_reply` | 本场见到非mock模型正文，仅作任务收益假设，换场清除 |
| `Game::observe/interpreted_events` | 本队局部感知、公告后延迟交付的正确解释夹具；不是生产事件解析器 |
| `Game::step/refresh_effects/resurrect/current_walls` | 整Tick结算、重建效果、按位复活、合成基础和临时墙 |
| `Game::score/result` | 规则原始分及逐车R/D/M、死亡和动作统计 |
| `relative_entity/make_scenario` | 生成89字符相对图、不同地图任务事件 |
| `Opponent::act/grid_offset` | 只用本队感知作风格决策、解析自己的相对实体位置 |
| `arena_main::configure/play/main` | 只改白名单策略权重、运行一局、读取离线JSON并逐局输出；无ROS参数入口 |
| Python`scenes/build/evaluate/summarize` | 组场、编译/规则测试、跑指定参数与场景、汇总指标 |
| Python`candidates/main` | 固定种子生成搜索候选、训练筛选并一次留出对照，保存源码哈希和结果 |

循环索引约定：`player/seat/i`是车，0–3；战术0总是本车。`mode`是手写情景，`depth`是本地推演Tick，`trial`是路线修改次数，`direction/d`是上右下左。`source/target`在伤害矩阵中是贡献者/受害者；`state/child/branch`均是推演副本，不是裁判感知。`normal/high`为计划拾取次数，`r/d`为计划R/D；配置含义在`match_config.hpp`逐项注释。

## 导航进展与脱困变量

`observe_navigation`在本帧地图更新后读取官方R/D/任务、HP、新格子数量及真实堡垒进度。`NavigationSample`只记可信连续Tick、实际坐标和上一目标；`recent_navigation_`至多保留一个配置窗口。`navigation_tick_/progress_tick_`分别记录上次可信回合和最近进展；`navigation_r_/d_/hp_/known_/missions_/capture_`是上一帧的对照，不是预测。

`loop_cells_`是触发时反复经过的格子；`recovery_goal_/until_`是脱困目的地与绝对截止。`recovery_blocked_`表示检查后没有安全出口，保留原循环区防止隔一会又空跑；`recovery_count_`记录本场真正触发次数。`deferred_goals_`把目标类别/坐标暂缓到指定Tick，不改变格子可走性。换场、断帧/不可信回执和复活的失效规则见NAVIGATION。

`decide`中的`available`检查脱困目标在当前BFS内可达且能在期限内到达；`best_escape`按新视野、低访问、有限距离与公开席位选择目标。仍只调用原`move`发布首步并处理幻术。`navigation_diagnostics`生成只读日志，经Session传给Bot；`navigation`是其JSON文本，含无进展时长、次数、目标、暂缓数和无出口标志。

导航夹具`fixture/run`只在测试端建场和推进；`positions/repeats/longest`统计无成绩进展的周期运动，`first_resource/first_task`记录首次实际达成Tick，`reasons/trace`保留行为证据。`run_unstuck.py`的`build/run/verify/main`依次编译同夹具、执行、检查六个500Tick结果、保存摘要；旧版生产核心只读，不被新版策略当输入。

## 公告前置与诊断入口

`Strategy::event_diagnostics(tick)`只读统计已校验事件的pending/active/expired及本车重建效果。`Session::EventFlow`记录本场回包、解释校验、合并/迟到和最近事件摘要；`Session::event_diagnostics()`合并两者。`AiWorker::Job/Result::request_tick`随后台任务保存来源逻辑Tick，不能用返回时刻重置事件。

`Bot::event_session_`与`logged_event_replies_`让event_update只在换场/新回包时输出；逐Tick行保留事件数和无双标志。`run.sh`的rmtc_run_log是本次控制台输出文件，pipeline保留机器人失败退出码。模型原文、思考、令牌均不打印。

未来堡垒路径使用arrival（本次规划到达Tick）、first_capture（开始驻留Tick）、needed（尚需结算次数）、finish（授予Tick）、wait（早到等待成本）。未激活不提前计算驻留，生效后继续原有capture_fortress/hold_fortress逻辑；prepare_fortress仅表示赶往已公告的未来堡垒。`tests/announcement_benchmark.cpp`是独立旧新核心配对夹具，解释由手工oracle延迟交付。
