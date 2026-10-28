# RoboMaster-Turing-Competition-BeyondControl

[上沿往返与争格脱困](NAVIGATION.md)：新增真实进展检测、目标暂缓和有期限的脱困目标；旧版上沿压力用例500Tick往返降至16Tick。未取得队友原地图，不能当作原场复盘。

[目标持续性、资源串搜索与探索](PLANNING.md)：已加入阶段结束守卫、目标迟滞、最多4目标/5候选的资源规划和信息增益探索；7个采集夹具已有同条件比较，完整对局仍待验收。
[赛前多风格对战与调参](ARENA.md)：六类地图、六种陪练、轮换席位、延迟解释及独立留出；连续受阻改道已启用。三Tick战术已实现，本轮训练选择关闭。自建测试不等于官方标准车结果。

RMTC 机甲大师图灵对决赛 C++17 / ROS 2 Humble 选手工程。

[2026-10-28实测](REAL_TEST.md)：真实DeepSeek经原版转发两次成功，发现短预告时延问题；已修复累积公告重复膨胀。完整裁判尝试因当前环境DDS权限失败，尚无对标准车的有效对局结果。

[规则状态机与路径分析](STATE_MACHINE.md)：四种动作权限、八类事件、逐条规则对应及一步对手预测。预测只调整收益，不改变当前战斗时序或确定占位。

赛前只改 [match_config.hpp](src/rmtc_bot/include/rmtc_bot/match_config.hpp)，改完重新编译。启动不需要输入参数或手工设置环境变量。

逐文件、函数、类型和变量说明见[源码导读](CODE_GUIDE.md)，代码声明与关键判断旁也有中文注释。

## 地址和凭据填在哪里

| 配置 | 正式比赛 | 本机真实LLM练习 |
| --- | --- | --- |
| `team` / `team_name` / `stage` | 本场席位、队名、阶段 | 自己占用的席位、队名、阶段 |
| `ros_domain_id` / `ros_localhost_only` / `rmw_implementation` | 按现场网络要求填 | 与本机练习裁判一致 |
| `ai_url` | 官方完整网关地址 | `http://127.0.0.1:8080/v1/chat/completions` |
| `ai_token` | 本场席位令牌 | A席位填`practice-A`，其他席位相应修改 |
| `ai_model` | 新公告统一`deepseek-flash` | 真实模型服务支持的模型名，必须填 |
| `ai_timeout_ms` / `ai_connect_timeout_ms` / `ai_max_tokens` | 本场超时和输出限制 | 当前明确为35000ms/5000ms/2200；HTTP在后台等待 |
| `ai_thinking` / `ai_json_output` | 新公告允许直接转发，明确`"disabled"` / `true` | 同样填写；不省略thinking以免默认开启 |
| `ai_http_proxy` | 机器人访问网关的网络代理；空串为直连 | 通常空串，机器人直连本机代理 |
| `practice_llm_url` | 不使用 | 真实模型服务的完整`/chat/completions`地址 |
| `practice_port` | 不使用 | 本机练习监听端口，须与`ai_url`中的端口一致 |
| `practice_llm_key` | 不使用 | 模型服务API Key，与`practice-A`不同 |
| `practice_llm_proxy` | 不使用 | 练习代理访问真实模型的网络代理；空串为直连 |

所有配置都在同一个头文件。当前写明了同机练习地址、`practice-A`、`deepseek-flash`及DeepSeek上游地址；上游Key留空，由你在本机填新密钥。正式赛按现场通知更换地址和令牌，模型保持新公告指定的`deepseek-flash`，明确关闭思考并请求JSON。第二阶段缺必要值就拒绝启动，没有隐式补值。

**模型名不放在URL中。** 机器人把`ai_model`放进POST正文，原版practice将它转给上游；请求带`model`时优先于`PRACTICE_MODEL`，没带时才用后者。我们的启动器也把同一个`ai_model`交给`PRACTICE_MODEL`，只需维护一处模型名。教程中的SDK `base_url`会自动补请求路径；这里使用curl，必须填写完整`/chat/completions`端点。

## 2026-10-28网关公告：关闭思考写在请求正文

按用户转来的新公告，正式网关除`model`外直接转发请求字段，不主动关闭思考；DeepSeek官方也说明思考默认开启。当前`AiWorker::observe`已经按下面形式序列化，`practice_launcher`和上游诊断共用同一配置，无需新增环境变量：

```json
{
  "model": "deepseek-flash",
  "thinking": {"type": "disabled"},
  "response_format": {"type": "json_object"},
  "messages": [{"role": "user", "content": "请只输出JSON，解析公告中的事件。"}],
  "max_tokens": 2200,
  "stream": false
}
```

`thinking`在HTTP JSON顶层。SDK中的`extra_body`是SDK传参方式，我们直接发HTTP，不能把它作为额外嵌套层。地址仍是本场完整`POST /v1/chat/completions`，请求头仍带本场`Authorization: Bearer ...`。模型名属于正文，和本机地址或练习上游地址分开。

公告的512是示例预算。当前保留2200用于多事件、证据原文和`unknown`，不把最大预算当作必然输出长度，也不认为调小它就保证低时延；实际请求仍受本场配额、并发和大小限制。降低前先检查多事件是否截断。JSON模式还要求Prompt明确要求JSON，完成后仍须本地校验场次、字段、证据与期限。

只读原版转发器的测试新增了512预算、thinking、response_format和temperature透传检查；C++客户端另检查新公告字段确实在顶层。上游是固定夹具，不代表正式网关或真实模型验收。关闭思考也不保证半秒完成，模型仍在后台解释，动作继续逐Tick生成。

依据：[DeepSeek思考模式](https://api-docs.deepseek.com/guides/thinking_mode/)与[Chat Completions字段](https://api-docs.deepseek.com/api/create-chat-completion/)，核对日期2026-10-28；赛事转发行为依据用户本次粘贴公告，尚未独立抓取正式网关。

## 两条通信链路

**ROS：裁判 → `/team_A/perception` → 机器人 → `/team_A/action` → 裁判。**
程序从本队感知读`tick`、`session_id`和真实坐标，再发布本Tick动作。感知接收使用DDS，不需要另填一个HTTP“接收网址”；域编号、局域网发现和防火墙按赛场要求配置。

**LLM：机器人 → `ai_url`网关 → 真实模型 → 原路返回机器人。**
`ai_url`是机器人发POST的接收端。机器人从同一条HTTP请求读取响应，不另外开回调端口。网关地址与网络代理地址是两回事；`ai_http_proxy`只决定怎样到达网关，不替代网关。

**动作JSON和模型JSON是两种结果。** 每Tick的`{tick,session_id,action,direction}`由本地策略按时生成；LLM只在有新公告时异步返回`{session_id,events,unknown}`，可跨多个Tick。收到后校验场次/证据/有效期，再影响后续动作，不拿旧Tick模型结果直接发布动作。规则v1.2 §11.1明确不要求逐Tick由AI生成动作；AI超时也不延长动作窗口。

本机练习时，网关就是原版`practice/`提供的8080代理；它再用`practice_llm_url/key`访问真实模型。正式比赛的转发由主办方管理，选手只配置官方网关，不填写主办方的上游模型密钥。

你的HelloCV笔记记录过WSL镜像网络和`127.0.0.1:7890`代理。如果当前机器确实依赖该代理出网，把`practice_llm_proxy`填为`http://127.0.0.1:7890`；机器人访问本机8080的`ai_http_proxy`仍填空串。启动器保留本机地址的`NO_PROXY`，陪练访问8080不绕出网代理。代理端口和WSL网络模式必须以当前机器为准，不能仅凭旧笔记断言7890仍在监听。

## 构建与启动

在WSL Ubuntu 22.04 / ROS 2 Humble环境中，先按[DEV.md](DEV.md)安装依赖。填写头文件后：

```bash
cd ~/cst
bash build.sh
```

更新配置前先用`Ctrl+C`停止旧机器人和旧练习裁判，再构建并重启两者。`run.sh`和`practice.sh`会检查本次构建的配置摘要；头文件改了但没构建时直接提示错误。启动固定使用`~/cst`自身的安装产物，终端会显示路径、构建时间、实际模型和思考模式。若日志仍打印旧模型，先处理旧进程/旧构建，不要继续改令牌。

本机练习开两个终端，均在`~/cst`：

```bash
# 终端一：启动原版练习裁判；第二阶段明确使用real模式。
bash practice.sh
```

```bash
# 终端二：启动自己的机器人。
bash run.sh
```

打开练习页面`http://127.0.0.1:8080`，确认自己的席位没有陪练占用，再开始比赛。第一阶段只需把头文件`stage`改为1并重新编译；不调用LLM。正式比赛只运行自己的`bash run.sh`，裁判由主办方启动。

`practice.sh`通过我们自己的启动器传入原版程序需要的配置，**不修改`practice/`任何文件**。点击开始前先确认页面的模式是`real`、席位A留给自己的程序。按裁判教程先只接自己的程序，确认后再勾B/C/D陪练，减少同席位冲突与其他调用干扰。

启动器也关闭Python字节码写入，避免测试刷新原版`__pycache__`。排查时同时保留选手终端的`ai_calls`日志；练习包里的陪练日志不能代替自己的C++选手日志。

## 第二阶段LLM成功为零，先看什么

| 日志 | 意味着什么 |
| --- | --- |
| 启动报缺`ai_url`、`ai_token`或`ai_model` | 填头文件、重新构建，还没开始调用 |
| `started=0` | 本客户端还未开始请求，不能把陪练记录当成自己的记录 |
| `last_http=400/401/429` | 分别检查请求/模型名、令牌、限流或配额状态 |
| `last_curl=28` | 请求超时；检查链路和头文件中的超时 |
| `ai=requesting` | 后台已开始HTTP请求；不再把在途状态叫queued |
| `first_byte_ms=0`且超时 | curl未取得首字节计时；不能单凭这项区分网关、上游网络或模型生成等待 |
| `model_reasoning_only` / `model_output_truncated` | 只有思考正文 / 输出被截断；检查思考模式、输出限制与延迟，不能把思考内容当事件答案 |
| `known_mock=true`、`ai=mock_gateway` | 收到固定模拟响应，没有验证真实LLM；以real模式重开练习 |
| `http_successes>0` | 完整HTTP请求成功；仍要核对模型输出、实际动作和裁判的本席位LLM记录 |

### HTTP 502：先检查练习代理的出网

`last_curl=0`且`last_http=502`表示本程序已经收到了网关的错误回复，不能直接说成客户端超时或模型思考太久。原版practice将连接/代理/TLS/读取异常、重定向及非JSON响应等情况包装为502；上游本身返回502也会透传。`ai.jsonl`只记状态和次数，不保存底层异常，仍不能单凭它区分原因。

更新代码后先构建，再运行一次最小上游检查；配置仍只取同一个头文件，不手输环境变量：

```bash
cd ~/cst
bash build.sh
bash check_ai.sh
```

该命令不需要ROS发现、不启动裁判，只用当前编译的`practice_llm_url/key/proxy`和模型选项直测一次最小JSON。沿用原转发器的5秒连接/25秒读取设置，不自动换代理或重试。它会产生一次真实上游请求，**不计本场LLM次数或正式资格**，仅供赛前练习排查。它拒绝旧启动器和过时配置，避免把诊断误当成裁判启动。

| `kind` / HTTP | 下一步 |
| --- | --- |
| `request_ok` | 当前最小请求成功；再重启已更新配置的裁判，检查完整公告请求。成功不保证大Prompt也及时 |
| `connect_timeout` / `connection_error` | 检查WSL出网、DNS和连接；若确实需要代理，只改`practice_llm_proxy`，不要改机器人访问本机8080的代理 |
| `proxy_error` | 检查所填代理是否在WSL可达、端口是否监听；程序不会自动猜7890 |
| `tls_error` | 检查证书与代理TLS链路，不能仅靠增加模型等待时间解决 |
| `read_timeout` | 库报告等待数据超时；TLS握手阶段也可能出现此标签，不能据此认定已进入模型推理，更不能当作模型思考时长 |
| `http_error` + 400/401/402/429/5xx | 分别核对请求参数、密钥、余额、限流和上游服务；此状态来自直测上游 |
| `redirect` | 填最终完整端点；不自动追随重定向 |
| `invalid_completion` / `empty_content` / `output_truncated` | 网络已取得回复，但JSON外壳、最终正文或输出上限不合要求 |

两段代理不同：启动日志里的`AI configured ... proxy=direct`只描述机器人→本机网关。练习代理→DeepSeek看`Practice ... upstream_proxy=...`，或本检查的`upstream_proxy`。**当`practice_llm_proxy`为空时，启动器会明确清空继承的HTTP代理设置**；终端其他程序能通过环境代理上网，不证明练习代理也在走它。修正配置后须重新构建，并停止、重启旧裁判与机器人。

检查返回`http_status=0`时，表示本脚本没有拿到可用完整响应，不是服务器返回了状态码0。先在同一WSL做不带Key、不调用模型的HTTPS检查，记录错误与DNS/TCP/TLS/首字节计时：

```bash
curl --noproxy '*' --connect-timeout 5 --max-time 15 -sS \
  -o /dev/null \
  -w '\nHTTP=%{http_code} DNS=%{time_namelookup}s TCP=%{time_connect}s TLS=%{time_appconnect}s FIRST=%{time_starttransfer}s TOTAL=%{time_total}s\n' \
  https://api.deepseek.com/models
```

收到401等HTTP回复至少证明取得了HTTPS响应，未带Key的401不代表真实Key失效；`HTTP=000`且TLS时间为0则HTTPS尚未完成。curl与Python仍可能在地址选择、证书或实现上不同，此检查只是缩小范围。其他机器通过代理调用成功，也不能替代本机直连验证。[最新真实复测及环境差别](REAL_TEST.md#临时key再次实测明确代理路径与wsl边界)已单独记录。

动作没有合适机会时，第二阶段存活车辆回退`DEFEND`；视野不足时可试探未知邻格。第一阶段或阵亡时不发送违法的防御动作。详细策略、源码注释和离线回归见[DEV.md](DEV.md)。

如果网页上A看起来没动，先看自己日志：同一`session`的`pos`是否变化，下一帧`last_action.moved`是否为`true`。`sent=move`只代表发布意图，`moved=true`才是裁判确认移动；`direction`是提交方向，`effective_direction`是实际方向。你提供的日志已有连续`moved=true`，不能把那段记录解释为整场没有执行移动。再核对浏览器的场次、Tick及是否打开了另一个练习进程；这些网页数据只用于人工排查，不输入机器人策略。

材料与修复对应关系见[MATERIALS_CHECK.md](MATERIALS_CHECK.md)。

