# 选手工程

工作空间 `~/cst`，C++17 / ROS 2 Humble，包名 `rmtc_bot`，可执行文件 `bot`。

## 安装与启动

```bash
sudo apt update
sudo apt install -y build-essential cmake python3-colcon-common-extensions nlohmann-json3-dev libcurl4-openssl-dev ros-humble-rclcpp ros-humble-std-msgs ros-humble-rmw-fastrtps-cpp
cd ~/cst
bash build.sh
```

先在另一个终端启动练习裁判，退出占用本席位的参考车。第一阶段：

```bash
bash run.sh A 'Frank' stage1
```

第二阶段练习（测试令牌仅用于公开练习代理）：

```bash
export RMTC_AI_URL='http://127.0.0.1:8080/v1/chat/completions'
export RMTC_TEAM_TOKEN='practice-A'
bash run.sh A 'Frank' stage2
```

两个参数的旧命令 `bash run.sh A 'Frank'` 默认 stage2。切换阶段、席位、更换令牌或重编译后重启机器人；已运行进程不会读取另一个终端中新设置的环境变量。

正式比赛把 `RMTC_AI_URL` 换成官方发放的完整 Chat Completions 地址，令牌换成本场席位令牌；模型及输出限制按当场通知设置：

```bash
read -rsp '本场席位令牌: ' RMTC_TEAM_TOKEN
export RMTC_TEAM_TOKEN
printf '\n'
# 按现场通知设置：RMTC_AI_URL、RMTC_AI_MODEL、RMTC_AI_MAX_TOKENS
bash run.sh A 'Frank' stage2
```

`RMTC_AI_MODEL` 未设置时省略 model，由网关使用默认模型。`RMTC_AI_MAX_TOKENS` 默认 2200，可设为 1–8192；必须服从官方公布的输出上限。网络请求超时默认 1800 ms，在后台线程执行。

ROS 网络仍读取 `ROS_DOMAIN_ID`、`ROS_LOCALHOST_ONLY`、`RMW_IMPLEMENTATION`，默认分别为 `27`、`0`、`rmw_fastrtps_cpp`，正式接入以现场值为准。缺少 AI 地址或令牌时程序会明确告警，继续运行本地策略；不能据此认定满足正式 AI 调用资格。

## 最小策略

| 部分 | 当前行为 |
| --- | --- |
| 公开感知与记忆 | 分别解析本车绝对坐标和实体 9×9 相对图；记住已看到的墙、资源和实际到达次数；当前视野中的新观测覆盖旧记录，视野外资源按时效降低优先级 |
| 采集与探索 | 在已知可通行格上 BFS，按价值、距离及未完成任务选资源；没有目标时探索较少走过的格子；未知格不直接当空地穿过 |
| 任务 | 读取结构化 missions 和最新 score.mission_completed_ids；处理普通/高价值资源、资源价值、有效伤害与 reach 目标；不重做已完成任务；远处未知任务点通过已知边界逐步接近 |
| 攻击与防御 | 按移动前位置判断直线 1–2 格最近敌车；墙不挡攻击、近处保护目标不被穿透；威胁可能致命且防御可保命时先防御，否则比较攻击、采集和任务机会 |
| LLM | 通过配置的官方兼容网关真实发送 HTTP 请求，后台解释公告，严格校验模型 JSON、场次、时间、坐标和公告原文证据；模型不能直接发布动作 |
| 时序与换场 | 新 Tick 重新决策；同 Tick 重发复用缓存动作，旧 Tick 与已退役场次丢弃；新场次清空地图、历史、效果及请求状态，跨场 HTTP 结果丢弃 |

第一阶段只执行 MOVE/WAIT，不发 AI 请求。第二阶段使用同一套基础规则并启用任务、攻防和已解释事件。

每次只执行路线第一步，下一帧采用实际 self.position 与 last_action 更新；不假设移动成功，不重复反转回执中的 effective_direction，也不缓存未来动作让其自动执行。

资源和战斗的直接收益按 v1.2 §10.2 的封顶前后分差计算，包括跨过 120/500 的部分收益；每帧重读允许下降、小数和超过 500 的官方 D。击杀独享估计为 60 D，辣椒损血不计普通伤害贡献；多人分奖与对手行动仍有不确定性。另有很小且有界的争夺/同分启发权重，不作为额外计分。

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
| src/rmtc_bot/src/strategy.cpp | 地图记忆、目标评分、BFS、攻防和效果状态 |
| src/rmtc_bot/src/events.cpp | 公告解释 Prompt 与模型输出校验 |
| src/rmtc_bot/src/ai_worker.cpp | curl 后台请求、公告缓存、限额与退避、跨场取消 |
| src/rmtc_bot/src/session.cpp | Tick/场次去重、缓存、采纳模型结果、异常等待回退 |
| src/rmtc_bot/src/bot.cpp | ROS 订阅/发布、注册和日志 |
| src/rmtc_bot/include/rmtc_bot/protocol.hpp | 感知信封与动作字段检查 |
| tests/ | 离线回归与本地 HTTP 协议夹具 |

## 错误与日志

429 quota_exceeded 后本场停止请求，429 rate_limit/concurrency_limit、连接失败、超时和非 JSON 服务错误按 1、2、4、8 秒退避；401/403/400/404 停止无意义重试。HTTP 2xx 但模型 JSON 不可用时仍保持本地策略，同一批公告最多尝试三次；新公告到达可再次尝试。

日志含 sent、reason、ai、http_success 和后续 last_action。http_success 只表示本程序收到本场一次 2xx 转发响应；模型结果还须通过校验，正式资格仍以裁判记录为准。令牌不写日志或命令行参数。

模型的 unknown 字段必须是字符串数组。存在未知项时只合并已校验的事件，标记 events_incomplete，按原退避和尝试次数上限重试；不能因为 JSON 有效或 HTTP 成功就标记整批公告已理解。后续补齐未知项后才标记 events_validated。

程序仅订阅自己的 /team_X/perception，其中已经含任务和近期公告；使用其公开字段和自己保存的历史，不读取观赛全图、练习配置、内部话题或对手专属输入。

## 验证

不需要 ROS 的完整回归（g++、nlohmann-json3-dev、libcurl4-openssl-dev、python3）：

```bash
cd ~/cst
bash tests/run.sh
```

开启内存/未定义行为检查：

```bash
RMTC_SANITIZE=1 bash tests/run.sh
```

tests/mock_gateway.py 仅在本机临时端口提供固定模拟模型响应，测试结束自动退出。测试不会请求正式网关，不能代替真实 LLM 效果或正式调用资格验证。CMake 的 BUILD_TESTING 也会注册不依赖 HTTP 服务的核心回归，可用 colcon test 运行。

回归覆盖采集/绕路/地图边界/缩小视野/重叠与畸形图、任务完成去重、攻防/保护/反伤、八类事件执行、跨死亡保留与到期、暂停/漏帧/换场，以及异步 HTTP、429 限流与额度耗尽、401、非 JSON 503、坏模型响应、超时与跨场取消。合成迷宫中 18 Tick 收集 3 个资源，移动未撞墙或越界。

独立核心使用 C++17、nlohmann/json 3.10.5 和 -Werror 检查。当前生成环境没有 ROS 2，未执行真实 ROS 构建、DDS 联调或正式网关调用；须在 WSL 中构建后，用真实 last_action、当前场次和网关记录完成联调。该环境的 ptrace 限制不支持 LeakSanitizer，执行 AddressSanitizer/UndefinedBehaviorSanitizer 时单独关闭了泄漏检测。

## 当前边界

这是覆盖全部主要流程的启发式基线，收益权重、对手下一步动作、对手隐藏增益和未见区域都含不确定性；当前 defending 不等于下一 Tick 防御。BFS 使用已知地图和有限的到达时刻检查，不是完整多车联合搜索或最优时空规划。没有完整历史时不虚构堡垒驻留或幻术资格。

模型可能漏解或误解公告，类型/边界/原文证据校验不能证明语义正确；尚未理解的效果不会被凭空套用。公开感知可能在断线时漏过公告，不能声称自动恢复全部历史。实战策略强度与复杂事件叠加仍需真实裁判对战验证。
