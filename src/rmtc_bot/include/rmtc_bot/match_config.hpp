#pragma once

// 比赛前只改这里，然后 bash build.sh。以下都是明确配置，不读环境变量、不猜缺失值。
namespace rmtc::match_config {
inline constexpr const char *team = "A";       // 裁判分配的本场席位。
inline constexpr const char *team_name = "Frank"; // 注册显示队名，不代替席位或令牌身份。
inline constexpr int stage = 2;               // 1：采集；2：对抗。
inline constexpr int stage1_ticks = 100;       // 正式规则§3：动作0–99，100是最终状态。
inline constexpr int stage2_ticks = 500;       // 正式规则§3：动作0–499，500是最终状态；改练习长度时在这里明确改。

inline constexpr const char *ros_domain_id = "27"; // 同一比赛ROS网络的域编号。
inline constexpr const char *ros_localhost_only = "0"; // 0允许局域网通信；1只在本机通信。
inline constexpr const char *rmw_implementation = "rmw_fastrtps_cpp"; // ROS底层DDS实现。

// 三项必须明确；这里按当前同机DeepSeek练习填写，正式赛须换成本场官方值。
// url 必须是完整的 /chat/completions 地址。正式赛填官方网关，不填模型服务主页。
// 同机练习填 http://127.0.0.1:8080/v1/chat/completions，token 填 practice-A；
// 练习裁判须另以 real 模式连接真实模型，mock 只验证通信。
inline constexpr const char *ai_url = "http://127.0.0.1:8080/v1/chat/completions";
inline constexpr const char *ai_token = "practice-A"; // 仅公开练习令牌；正式令牌不要提交。
inline constexpr const char *ai_model = "deepseek-flash"; // 请求体中的模型名；练习启动器同时设置PRACTICE_MODEL。
inline constexpr long ai_timeout_ms = 35000;  // 后台等完整响应；覆盖练习代理5秒连接+25秒读取，不阻塞动作。
inline constexpr long ai_connect_timeout_ms = 5000; // 建连单独限5秒；原来的500ms对出网/TLS过短。
inline constexpr int ai_max_tokens = 2200;    // 按当场输出限制修改。
inline constexpr const char *ai_http_proxy = ""; // 机器人→网关的网络代理；空串明确表示直连。
// 当前练习使用DeepSeek：明确关闭思考，直接生成事件JSON。空串表示不发送thinking字段。
// 正式赛按公布的模型能力填写；不支持扩展字段的网关填空串、json_output填false。
inline constexpr const char *ai_thinking = "disabled";
inline constexpr bool ai_json_output = true; // 明确请求JSON模式；Prompt也要求JSON，仍需本地校验。

// 只供 bash practice.sh 使用：练习代理→真实模型。正式赛不使用这组上游配置。
// 模型名共用上面的ai_model。真实服务密钥与practice-A是两种不同的凭据。
inline constexpr int practice_port = 8080;     // 原版练习代理监听端口，须与本机ai_url一致。
inline constexpr const char *practice_llm_url = "https://api.deepseek.com/chat/completions"; // DeepSeek文档给出的完整POST端点。
inline constexpr const char *practice_llm_key = "";
inline constexpr const char *practice_llm_proxy = ""; // 上游出网代理；如确用WSL本机7890，填http://127.0.0.1:7890。

// 下面只调策略取舍。攻击距离、伤害、计分公式和结算顺序按规则实现，不在这里调。
inline constexpr double stale_resource_decay = 0.01; // 资源记忆每Tick衰减系数。
inline constexpr double resource_mission_bonus = 5.0; // 未完成采集任务时的额外权重。
inline constexpr double reach_mission_value = 8.0;    // 到点任务权重，还会除以距离+1。
inline constexpr double healing_below_hp = 70.0;     // 低于此血量才主动找治疗点。
inline constexpr double fortress_hold_value = 20.0;  // 保住尚未完成的驻留进度。
inline constexpr double fortress_capture_value = 12.0; // 前往堡垒的收益分子。
inline constexpr int hazard_lead_ticks = 2;          // 比最短退出时间再提前几Tick。
inline constexpr double goal_retention_bonus = 0.10; // 同一有效目标给10%排序余量，抑制小波动；不是官方加分。
inline constexpr int resource_plan_depth = 3;       // 资源串最多扩展3个目标，不是完整战斗树或3Tick。
inline constexpr int resource_beam_width = 5;       // 每层最多保留5条未执行计划，优先保留不同首方向。
inline constexpr int resource_candidate_limit = 8;  // 先按现有收益/距离挑8个资源；地图资源数不写死。
inline constexpr int resource_plan_budget_ms = 10;  // 额外搜索软预算；耗尽时交回已算好的合法基线。
inline constexpr double frontier_value = 0.02;      // 每份潜在新视野的信息权重；不是资源分或必然有资源的概率。
inline constexpr double opponent_contest_cost = 1.0; // 稳定轨迹预计争同格时，给路线增加一份等待成本；启发式，非概率。
inline constexpr double opponent_pressure_cost = 0.25; // 下一Tick可能进入稳定轨迹敌车射线时的软成本，不改变攻击时序。
inline constexpr bool tactical_search = true;       // 可见敌车附近才比较最多三Tick、多个合法对手情景。
inline constexpr double tactical_risk = 0.35;        // 最差情景的权重；不是经训练的概率。
inline constexpr double tactical_hp_value = 0.015;   // 每HP的未来机会估值，不写入官方分数。
inline constexpr double tactical_death_cost = 1.0;   // 除规则D扣除外，死亡丢失位置/行动窗口的估值。
inline constexpr double tactical_future_discount = 0.65; // 第二Tick相对第一Tick的收益权重。
inline constexpr double tactical_goal_weight = 0.35; // 原目标评价在战术比较中的尾部价值。
}  // namespace rmtc::match_config

