# 选手工程

工作空间 `~/cst`，C++17 / ROS 2 Humble，包名 `rmtc_bot`，可执行文件 `bot`。

## 安装与启动

```bash
sudo apt update
sudo apt install -y build-essential cmake python3-colcon-common-extensions nlohmann-json3-dev libcurl4-openssl-dev ros-humble-rclcpp ros-humble-std-msgs ros-humble-rmw-fastrtps-cpp
cd ~/cst
bash build.sh
```

赛前只改 `src/rmtc_bot/include/rmtc_bot/match_config.hpp`：席位、队名、阶段、ROS网络、AI地址/令牌/模型/超时/输出长度及策略权重都在这里。没有启动参数或环境变量兜底。改完重新编译，再运行：

```bash
bash build.sh
bash run.sh
```

第二阶段的 `ai_url`、`ai_token`、`ai_model` 必须填，留空会拒绝启动。URL必须是完整的 `/chat/completions` 地址，不能只填服务主页。超时和输出长度也取配置中的明确值；请求在后台等待，不阻塞动作回调。

正式比赛填本场官方网关、席位令牌和指定模型。练习时机器人填本地代理 `http://127.0.0.1:8080/v1/chat/completions` 和 `practice-A`（按席位改），同一头文件还提供`practice_llm_url/key/proxy`；`bash practice.sh`将这些配置交给原版练习程序，第二阶段明确使用real模式，不修改`practice/`。更换配置、阶段或席位后必须重新编译并重启。地址关系和完整操作步骤见[README.md](README.md)。

## 最小策略

| 部分 | 当前行为 |
| --- | --- |
| 公开感知与记忆 | 分别解析本车绝对坐标和实体 9×9 相对图；记住已看到的墙、资源和实际到达次数；当前视野中的新观测覆盖旧记录，视野外资源按时效降低优先级 |
| 采集与探索 | 在已知可通行格上 BFS，按价值、距离及未完成任务选资源；没有明确目标时按潜在新增可见格/距离探索，完全已知区域再选少访问格；未知格不直接当空地穿过 |
| 任务 | 读取结构化 missions 和最新 score.mission_completed_ids；处理普通/高价值资源、资源价值、有效伤害与 reach 目标；不重做已完成任务；远处未知任务点通过已知边界逐步接近 |
| 攻击与防御 | 按移动前位置判断直线 1–2 格最近敌车；墙不挡攻击、近处保护目标不被穿透；威胁可能致命且防御可保命时先防御，否则比较攻击、采集和任务机会 |
| LLM | 通过配置的官方兼容网关真实发送 HTTP 请求，后台解释公告，严格校验模型 JSON、场次、时间、坐标和公告原文证据；模型不能直接发布动作 |
| 时序与换场 | 新 Tick 重新决策；同 Tick 重发复用缓存动作，旧 Tick 与已退役场次丢弃；新场次清空地图、历史、效果及请求状态，跨场 HTTP 结果丢弃 |

第一阶段只提交MOVE，不发AI请求。第二阶段使用同一套基础规则并启用任务、攻防和已解释事件。第二阶段存活时无目标/异常回退DEFEND，驻留或等危险结束也用DEFEND；阵亡、动作前必死、分裂强制等待以及第一阶段无合法动作时不发布。程序不生成默认WAIT；未提交时裁判仍按自身规则结算。

没有已知路线且邻格尚未探明时，优先尝试地图内、没有已知占位/墙/公告危险的邻格。失败后优先换一个较少尝试的方向，绝不自行把未知格标成空地或把本车坐标加一步。普通移动失败不增加无效动作；错误场次、格式或状态仍须避免。

每次只执行路线第一步，下一帧采用实际 self.position 与 last_action 更新；不假设移动成功，不重复反转回执中的 effective_direction，也不缓存未来动作让其自动执行。

公告含近期爆炸或分裂时，额外在当前位置 8 格内、最多 145 个已知可通行格上检查未来 8 Tick 的移动和等待。若能找到避开已知危险的后续路径，先过滤无法接续的第一步，避免最近出口成为随后被封死的陷阱；爆炸按移动前结算，允许在其已结算后进入该格。找不到可认证路线时仍使用原有合法动作回退。这只检查已知环境危险，不保证对手不争格、未知地图安全或 8 Tick 后存活。

资源和战斗的直接收益按 v1.2 §10.2 的封顶前后分差计算，包括跨过 120/500 的部分收益；每帧重读允许下降、小数和超过 500 的官方 D。击杀独享估计为 60 D，辣椒损血不计普通伤害贡献；多人分奖与对手行动仍有不确定性。另有很小且有界的争夺/同分启发权重，不作为额外计分。

少量赛前参数也集中在 `src/rmtc_bot/include/rmtc_bot/match_config.hpp`：资源记忆衰减、资源任务奖励、reach 任务价值、求治疗血量阈值、堡垒驻留/前往价值和避险提前量。它们是启发式权衡，当前数值与集中前一致；官方伤害、攻击距离、得分封顶和结算顺序不属于调参项。修改头文件后重新编译，再使用不同地图、席位和对手组合评估，保留独立验证场景，最后封存选定版本。运行时没有调参或热更新；目前尚未建立真实裁判自动对战、统计与参数搜索流程，离线回归通过也不代表找到最佳参数。

## 八类事件

事件机制按赛事规则 v1.2，输入格式见 [公开通信协议](practice/CONTESTANT_PROTOCOL.md) §3–5。区域、点位、起止 Tick 及效果参数由模型解释当场公告，下面都是本地执行逻辑。

| 类型 | 行为 |
| --- | --- |
| 爆炸 | 在激活前选择已知逃离路径；激活 Tick 的伤害发生在移动前，不能把当前 MOVE 当作补救；保护免疫单独判断 |
| 幻术 | 依据激活 Tick 已记录的位置确定是否受影响；仅补偿 MOVE 方向；离开区域/死亡不清除，绝对到期；晚加入且缺少激活位置时不猜测方向 |
| 天使恩泽 | 低血量优先成功入点，按真实移动回执记录回血期限；生成时站在点上或等待不算入点 |
| 天下无双 | 使用公告区域，抵达后驻留；从已经结算且连续的实际回执累计，战斗后达标后才使用 5×5 攻击；未完成进度被离开/死亡打断，已获得增益保留到结束 |
| 世界分裂 | 提前离开公告十字线，生效期间视为临时墙；自己已知的初始出生点作为例外；已知激活死亡/强制等待不伪造移动；到期的未复核墙退回未知 |
| 火爆辣椒 | 评估入点火线的残血收割机会，也可离开再进入；对手可能踩点时保守避开致命火线，不把预测当作已执行事实 |
| 洁身自好 | 评估攻击防御目标的反伤及自身生命风险，防御可能抵挡攻击并反伤；不把上一帧 defending 当作下一 Tick 承诺 |
| 潘潘的神 | 只由触发窗口内的真实成功移动激活易伤，从下一 Tick 战斗开始，死亡不延期，按公告统一到期 |

模型输出使用本地 events schema，不假称裁判提供了结构化随机事件字段。不使用关键词、正则或固定赛程解释公告。允许同 Tick 多事件；相同类型、激活时间与几何范围的解释合并，重复公告不会重复累计效果。

## 代码位置

| 文件 | 用途 |
| --- | --- |
| src/rmtc_bot/include/rmtc_bot/match_config.hpp | 比赛前统一配置 |
| CODE_GUIDE.md | 文件、命名空间、类型、变量和函数主线导读 |
| src/rmtc_bot/src/practice_launcher.cpp | 只给原版练习程序传配置并启动，不修改practice/ |
| src/rmtc_bot/src/strategy.cpp | 地图记忆、目标评分、BFS、攻防和效果状态 |
| src/rmtc_bot/src/events.cpp | 公告解释 Prompt 与模型输出校验 |
| src/rmtc_bot/src/ai_worker.cpp | curl 后台请求、公告缓存、限额与退避、跨场取消 |
| src/rmtc_bot/src/session.cpp | Tick/场次去重、缓存、采纳模型结果、防御或不发布回退 |
| src/rmtc_bot/src/bot.cpp | ROS 订阅/发布、注册和日志 |
| src/rmtc_bot/include/rmtc_bot/protocol.hpp | 感知信封与动作字段检查 |
| tests/ | 离线回归与本地 HTTP 协议夹具 |

## 错误与日志

429 quota_exceeded 后本场停止请求，429 rate_limit/concurrency_limit、连接失败、超时和非 JSON 服务错误按 1、2、4、8 秒退避；401/403/400/404 停止无意义重试。HTTP 2xx 但模型 JSON 不可用时仍保持本地策略，同一批公告最多尝试三次；新公告到达可再次尝试。

日志含 session、tick、pos、sent、direction、reason、ai、http_success 和后续 last_action。pos取裁判的self.position；sent/direction是当前意图，last_action是已经结算的反馈。即使本Tick不发布动作也记录当前状态和AI诊断。http_success只表示本程序收到本场一次完整2xx响应；模型结果还须通过校验，正式资格仍以裁判记录为准。令牌不写日志或命令行参数。

`ai_calls`直接显示本场开始请求数`started`、完成数`finished`、完整HTTP成功数`http_successes`、最近HTTP状态`last_http`、curl错误码`last_curl`和是否识别到练习模拟响应`known_mock`。`started=0`说明本客户端还未开始请求；`last_curl=28`是超时。明确的`practice-mock`响应标为`mock_gateway`，停止本场无意义重试，不采纳其中的事件。HTTP成功、事件校验通过、真实模型调用和正式资格是不同证据；没有模拟标记也不能单凭客户端证明资格。

新增`elapsed_ms/connect_ms/first_byte_ms/last_result`保留最近完成请求的总耗时、连接耗时、首字节时刻和原始结果类别。在途状态明确为`requesting`；第一字节计时为0且超时，只能说明未取得首字节计时，不能直接判定是哪一段网络坏了。

当前DeepSeek练习明确发送`thinking.type=disabled`和`response_format.type=json_object`。两项都在头文件中配置；不支持的正式网关可明确省略，没有报400后自动换参数的逻辑。总请求35000ms、连接5000ms，避免旧10秒客户端先于原版代理的5秒连接/25秒读取上限放弃；真实模型是否能及时完成仍需实测。`finish_reason=length`、仅reasoning_content、空答案和坏Chat Completions外壳分别诊断，不采用思考正文或补齐截断事件。

模型最终正文可以带Markdown围栏、前后说明或字符串外尾逗号；仅整理完整JSON对象，然后照常检查session、证据、坐标和字段。拒绝多个对象、缺括号、缺必填字段和错场次；不从公告自然语言提取规则，不修补事件语义。

模型的 unknown 字段必须是字符串数组。存在未知项时只合并已校验的事件，标记 events_incomplete，按原退避和尝试次数上限重试；不能因为 JSON 有效或 HTTP 成功就标记整批公告已理解。后续补齐未知项后才标记 events_validated。

程序仅订阅自己的 /team_X/perception，其中已经含任务和近期公告；使用其公开字段和自己保存的历史，不读取观赛全图、练习配置、内部话题或对手专属输入。

## 验证

不需要ROS的回归依赖g++、nlohmann-json3-dev、libcurl4-openssl-dev、python3；只读运行原版转发器与上游诊断夹具另需python3-requests（原练习环境已要求安装）。

```bash
cd ~/cst
bash tests/run.sh
```

开启内存/未定义行为检查：

```bash
RMTC_SANITIZE=1 bash tests/run.sh
```

tests/mock_gateway.py 仅在本机临时端口提供固定模拟模型响应，测试结束自动退出。测试不会请求正式网关，不能代替真实 LLM 效果或正式调用资格验证。CMake 的 BUILD_TESTING 也会注册不依赖 HTTP 服务的核心回归，可用 colcon test 运行。

`tests/startup_test.py`检查旧配置摘要拦截、固定工作空间执行路径及启动器设置的本机绕过/出网代理环境；系统ROS仅在临时脚本副本中用空操作替代。`tests/practice_gateway.py`校验原版practice.py对象哈希，只读加载其原始HTTP处理器；ROS导入用占位，裁判节点不启动。真实C++ Session经这个原始处理器转发至固定上游夹具，再检查事件进入策略、A席位记录、model优先级、令牌替换和代理路径。夹具模式名虽为real，仍不代表调用了真实模型。完整仓库中`tests/run.sh`自动运行该项；原文件不在本地时明确打印跳过。

可以用`bash tests/run.sh /只读原件路径/practice.py`指定同一发行原件。此参数只供测试，生产启动配置仍集中在头文件。

回归覆盖采集/绕路/地图边界/缩小视野/重叠与畸形图、任务完成去重、攻防/保护/反伤、八类事件执行、跨死亡保留与到期、暂停/漏帧/换场，以及异步 HTTP、429 限流与额度耗尽、401、非 JSON 503、坏模型响应、超时与跨场取消。合成迷宫中 18 Tick 收集 3 个资源，移动未撞墙或越界。

独立核心使用 C++17、nlohmann/json 3.10.5 和 -Werror 检查。当前主机未标准安装ROS/colcon；已通过隔离Humble依赖手动链接实际节点，真实DeepSeek链路此前有两次成功。完整裁判因DDS接口权限未能开局，WSL/colcon和正式资格仍须实测，详见[REAL_TEST.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/REAL_TEST.md)。该环境的 ptrace 限制不支持 LeakSanitizer，执行 AddressSanitizer/UndefinedBehaviorSanitizer 时单独关闭了泄漏检测。

## 当前边界

这是覆盖全部主要流程的启发式基线，收益权重、对手下一步动作、对手隐藏增益和未见区域都含不确定性；当前 defending 不等于下一 Tick 防御。BFS 使用已知地图和有限的到达时刻检查，不是完整多车联合搜索或最优时空规划。没有完整历史时不虚构堡垒驻留或幻术资格。

模型可能漏解或误解公告，类型/边界/原文证据校验不能证明语义正确；尚未理解的效果不会被凭空套用。公开感知可能在断线时漏过公告，不能声称自动恢复全部历史。实战策略强度与复杂事件叠加仍需真实裁判对战验证。

## 新增短期规划

目标迟滞、资源串Beam、动态视野探索、剩余Tick边界和配对夹具见[PLANNING.md](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/docs/implementation/PLANNING.md)。深度3表示资源目标数，未实现完整多车攻防联合推演。所有新增取舍仍集中在match_config.hpp，正式伤害与计分规则不调参。


## 2026-10-28多风格对战与当前比赛选项

[ARENA.md](ARENA.md)记录六地图/六陪练、两轮训练与独立留出。当前封存参数在`match_config.hpp`：资源任务4、到点4、治疗阈值40HP、目标深度4；连续两次受阻后改道。三Tick战术虽然实现并有规则回归，本轮训练选择关闭，不能把功能数当成绩。路线反转/插入/删除没有在本批消融中带来成绩变化。

`bash arena.sh --tune`是赛前工具，不读密钥、不改practice、不热更新参数；`bash tests/run.sh`和CTest也包含本地裁判规则边界。规则常量不参加搜索。模拟AI资格/延迟解释不是实际模型调用；完整标准车验收仍受当前DDS环境限制。参数与成绩详见[结构化结果](https://github.com/Frank-678/The-Logical-Structure-of-RMT-C/blob/56da8b3de414635bb0cfced3bdf90fcf68409957/checks/code-results/arena-2026-10-28.json)。
