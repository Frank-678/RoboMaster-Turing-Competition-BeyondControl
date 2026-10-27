# 源码导读

先读比赛配置，再沿“收感知→管场次→作决策→发动作”阅读。变量的具体含义写在声明旁，关键规则写在判断旁。

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

| 文件 | 职责 |
| --- | --- |
| `README.md` | 先看这里：地址、令牌、模型分别填哪，两个终端如何启动，零次调用如何查 |
| `DEV.md` | 依赖、策略机制、事件表、测试方法和当前验证范围 |
| `CODE_GUIDE.md` | 本文件：主线、文件、类型和函数索引 |
| `MATERIALS_CHECK.md` | 原始材料和已有学习笔记具体对应哪些修复，以及验证范围 |
| `LICENSE` | 本工程许可证 |
| `.gitignore` | 排除构建、安装和日志等产物 |
| `build.sh` | 在正确工作空间构建选手包，生成编辑器编译数据库 |
| `run.sh` | 加载ROS环境，启动已构建的机器人；配置不从命令行或环境补值 |
| `practice.sh` | 启动我们自己的练习配置入口，原版`practice/`不变 |
| `src/rmtc_bot/package.xml` | ROS包名、版本、构建和运行依赖 |
| `src/rmtc_bot/CMakeLists.txt` | 构建核心库、ROS入口、练习启动器和核心测试，安装可执行文件 |
| `include/rmtc_bot/match_config.hpp` | 唯一赛前配置：身份、阶段、ROS、两段AI链路、权重和阈值 |
| `include/rmtc_bot/strategy_tuning.hpp` | 旧include路径的兼容入口，只包含`match_config.hpp`，没有第二份参数 |
| `include/rmtc_bot/strategy.hpp` | 策略接口、地图格/历史快照和跨Tick记忆 |
| `src/strategy.cpp` | 实际决策：地图、路径、任务、攻防、八类事件和未知格试探 |
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
| 协议 | `frame_tick`验证感知信封；`action_for`检查提案并回填身份，返回合法动作或null |
| 坐标工具 | `integer`检查整数；`absolute_position`读[x,y]；`relative_position`读实体图；`manhattan`算四向距离；`ray`判断普通攻击方向 |
| 策略状态 | 构造函数固定阶段；`reset`换场清记忆；`merge_events`合并解释；`known_events`生成LLM上下文；`reason`返回决策原因 |
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
