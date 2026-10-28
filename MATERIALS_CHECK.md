# 原始材料怎样对应到这次修复

本页解释采用了什么，不把文件清单、静态阅读或模拟测试称为真实比赛通过。原版`practice/`继续只读。

| 材料 | 对照内容 | 代码中的对应 |
| --- | --- | --- |
| 原始《裁判测试端使用教程v1.1》PDF第3–4页 | 收感知、发动作、下一帧执行回执，必须验证双向 | 日志增加`session/pos/direction`；不再只看sent或网页印象判断移动 |
| 同PDF第6页（已核查原页） | 本机8080 → 上游完整地址；请求model优先于PRACTICE_MODEL；非流式；先只接自己的程序 | 一个`ai_model`同时用于请求体和原启动器环境；原版处理器集成测试验证优先级及A席位计数 |
| 同PDF第7–9页 | 部署与重开创建场次；同席位不能有两个控制程序；SSH只转HTTP、不转ROS | 启动步骤明确停止旧进程、重开练习、核对场次；原有Session换场/去重继续生效 |
| 原始《通信协议v1.1》PDF第14–15页（已核查原页） | 完整Chat Completions端点、Content-Length、非流式、错误码与超时处理 | 保留明确model、长度、Bearer和单后台请求；增加连接/首字节/总耗时及最终答案错误分类 |
| 原始《赛事规则v1.2》PDF第19页§11.1（已核查原页） | 不要求逐Tick由AI生成动作；模型超时不延长动作窗口 | 动作JSON每Tick由本地生成；LLM异步生成事件JSON，可以跨Tick，校验后才更新后续状态 |
| [HelloCV的LLM实践](https://github.com/Frank-678/HelloCV/blob/main/doc/LLM实践任务.md) | 请求格式、围栏/前后说明、空content、reasoning_content、finish_reason、分层校验 | 最终正文做有限JSON整理；只思考/截断回复拒绝；HTTP完成和事件验证分别记录 |
| [HelloCV](https://github.com/Frank-678/HelloCV)的Linux与ROS2笔记 | WSL镜像网络、7890代理、本机NO_PROXY；域/QoS/overlay；按证据链调试 | 修复练习启动器清空NO_PROXY；固定当前工作空间可执行路径；检查配置摘要，启动显示构建时间和实际设置 |
| 赛前规则、架构与战术分析 | 模型异步解释，校验后更新效果，本地按真实坐标持续行动；不能猜地图或事件参数 | 这些已有主线保留；这次修复调用链和可观察性，不引入地图真值、关键词事件解析或模型直接动作 |

前期培训的DeepSeek直连实验用于理解HTTP，不替代正式比赛的官方网关要求。模型服务的SDK `base_url`和curl的完整POST地址也不是同一个配置值。

## 这次运行日志能证明什么

- 多帧`last_action.moved=true`是裁判确认移动；不能据此说A整段完全没执行动作。继续比较同场`self.position`，再人工核对网页。
- `last_curl=28,last_http=0`证明客户端超时且没有HTTP状态码，不能单凭它区分网关、上游出网、模型等待或其他链路问题。
- 运行日志模型和所贴头文件模型不同，证明该进程没用这份配置；新脚本在头文件改变后拒绝旧构建，仍须停止已经在运行的旧进程。
- 旧客户端10秒上限先于原练习代理5秒连接/25秒读取上限。调整为35秒并关闭当前DeepSeek练习的思考模式，是对时序的修复，不是对这台WSL网络原因的最终判定。

当前DeepSeek接口选项依据官方[思考模式](https://api-docs.deepseek.com/guides/thinking_mode/)、[JSON输出](https://api-docs.deepseek.com/guides/json_mode/)及[接口文档](https://api-docs.deepseek.com/api/create-chat-completion/)，核对日期2026-10-27。模型名采用明确的`deepseek-flash`，没有按名称猜测能力或自动更换模型。

## 实际验证范围

`bash tests/run.sh`验证同一C++核心及HTTP客户端；完整仓库另只读加载原版HTTP处理器，验证转发、model优先级、凭据替换、本机绕过/出网代理及A席位记录。原件哈希、内存检查与具体结果记在PR中。

2026-10-27的这一集成测试上游是固定HTTP夹具；即使原处理器记录mode=real，也不能作为真实LLM或正式AI资格证据，ROS导入用占位。

2026-10-28新增两次真实DeepSeek调用、实际Humble节点构建及原版裁判/陪练启动尝试；修复累积公告重复追加。真实调用已成功，但完整对局因执行环境DDS权限失败，尚无标准车比较结果。耗时、证据及边界见[实测记录](REAL_TEST.md)。

