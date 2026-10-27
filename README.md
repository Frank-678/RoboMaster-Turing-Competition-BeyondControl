# RoboMaster-Turing-Competition-BeyondControl
RMTC 机甲大师图灵对决赛 C++17 / ROS 2 Humble 选手工程。

赛前只改 [match_config.hpp](src/rmtc_bot/include/rmtc_bot/match_config.hpp)，改完重新编译。启动不需要输入参数或手工设置环境变量。

逐文件、函数、类型和变量说明见[源码导读](CODE_GUIDE.md)，代码声明与关键判断旁也有中文注释。

## 地址和凭据填在哪里

| 配置 | 正式比赛 | 本机真实LLM练习 |
| --- | --- | --- |
| `team` / `team_name` / `stage` | 本场席位、队名、阶段 | 自己占用的席位、队名、阶段 |
| `ros_domain_id` / `ros_localhost_only` / `rmw_implementation` | 按现场网络要求填 | 与本机练习裁判一致 |
| `ai_url` | 官方完整网关地址 | `http://127.0.0.1:8080/v1/chat/completions` |
| `ai_token` | 本场席位令牌 | A席位填`practice-A`，其他席位相应修改 |
| `ai_model` | 主办方指定的模型名 | 真实模型服务支持的模型名，必须填 |
| `ai_timeout_ms` / `ai_max_tokens` | 本场超时和输出限制 | 明确填写，当前配置为10000ms/2200 |
| `ai_http_proxy` | 机器人访问网关的网络代理；空串为直连 | 通常空串，机器人直连本机代理 |
| `practice_llm_url` | 不使用 | 真实模型服务的完整`/chat/completions`地址 |
| `practice_port` | 不使用 | 本机练习监听端口，须与`ai_url`中的端口一致 |
| `practice_llm_key` | 不使用 | 模型服务API Key，与`practice-A`不同 |
| `practice_llm_proxy` | 不使用 | 练习代理访问真实模型的网络代理；空串为直连 |

所有配置都在同一个头文件。URL、令牌、模型名不会自动补值，第二阶段留空就拒绝启动。令牌和真实服务密钥只在自己的机器填写，不要提交到仓库。

## 两条通信链路

**ROS：裁判 → `/team_A/perception` → 机器人 → `/team_A/action` → 裁判。**
程序从本队感知读`tick`、`session_id`和真实坐标，再发布本Tick动作。感知接收使用DDS，不需要另填一个HTTP“接收网址”；域编号、局域网发现和防火墙按赛场要求配置。

**LLM：机器人 → `ai_url`网关 → 真实模型 → 原路返回机器人。**
`ai_url`是机器人发POST的接收端。机器人从同一条HTTP请求读取响应，不另外开回调端口。网关地址与网络代理地址是两回事；`ai_http_proxy`只决定怎样到达网关，不替代网关。

本机练习时，网关就是原版`practice/`提供的8080代理；它再用`practice_llm_url/key`访问真实模型。正式比赛的转发由主办方管理，选手只配置官方网关，不填写主办方的上游模型密钥。

## 构建与启动

在WSL Ubuntu 22.04 / ROS 2 Humble环境中，先按[DEV.md](DEV.md)安装依赖。填写头文件后：

```bash
cd ~/cst
bash build.sh
```

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

`practice.sh`通过我们自己的启动器传入原版程序需要的配置，**不修改`practice/`任何文件**。修改头文件后必须重新编译并重启对应进程。

启动器也关闭Python字节码写入，避免测试刷新原版`__pycache__`。排查时同时保留选手终端的`ai_calls`日志；练习包里的陪练日志不能代替自己的C++选手日志。

## 第二阶段LLM成功为零，先看什么

| 日志 | 意味着什么 |
| --- | --- |
| 启动报缺`ai_url`、`ai_token`或`ai_model` | 填头文件、重新构建，还没开始调用 |
| `started=0` | 本客户端还未开始请求，不能把陪练记录当成自己的记录 |
| `last_http=400/401/429` | 分别检查请求/模型名、令牌、限流或配额状态 |
| `last_curl=28` | 请求超时；检查链路和头文件中的超时 |
| `known_mock=true`、`ai=mock_gateway` | 收到固定模拟响应，没有验证真实LLM；以real模式重开练习 |
| `http_successes>0` | 完整HTTP请求成功；仍要核对模型输出、实际动作和裁判的本席位LLM记录 |

动作没有合适机会时，第二阶段存活车辆回退`DEFEND`；视野不足时可试探未知邻格。第一阶段或阵亡时不发送违法的防御动作。详细策略、源码注释和离线回归见[DEV.md](DEV.md)。
