#pragma once

// 比赛前只改这里，然后 bash build.sh。以下都是明确配置，不读环境变量、不猜缺失值。
namespace rmtc::match_config {
inline constexpr const char *team = "A";       // 裁判分配的本场席位。
inline constexpr const char *team_name = "Frank";
inline constexpr int stage = 2;               // 1：采集；2：对抗。

inline constexpr const char *ros_domain_id = "27";
inline constexpr const char *ros_localhost_only = "0";
inline constexpr const char *rmw_implementation = "rmw_fastrtps_cpp";

// 三项必须填；第二阶段留空就拒绝启动，防止又跑完整场才发现零次调用。
// url 必须是完整的 /chat/completions 地址。正式赛填官方网关，不填模型服务主页。
// 同机练习填 http://127.0.0.1:8080/v1/chat/completions，token 填 practice-A；
// 练习裁判须另以 real 模式连接真实模型，mock 只验证通信。
inline constexpr const char *ai_url = "";
inline constexpr const char *ai_token = "";    // 本场令牌；填入真实令牌后不要提交此内容。
inline constexpr const char *ai_model = "";    // 使用赛前公布的模型名。
inline constexpr long ai_timeout_ms = 10000;  // 后台最多等10秒，不阻塞500ms的动作窗口。
inline constexpr int ai_max_tokens = 2200;    // 按当场输出限制修改。

// 下面只调策略取舍。攻击距离、伤害、计分公式和结算顺序按规则实现，不在这里调。
inline constexpr double stale_resource_decay = 0.01; // 资源记忆每Tick衰减系数。
inline constexpr double resource_mission_bonus = 5.0; // 未完成采集任务时的额外权重。
inline constexpr double reach_mission_value = 8.0;    // 到点任务权重，还会除以距离+1。
inline constexpr double healing_below_hp = 70.0;     // 低于此血量才主动找治疗点。
inline constexpr double fortress_hold_value = 20.0;  // 保住尚未完成的驻留进度。
inline constexpr double fortress_capture_value = 12.0; // 前往堡垒的收益分子。
inline constexpr int hazard_lead_ticks = 2;          // 比最短退出时间再提前几Tick。
inline constexpr double opponent_contest_cost = 1.0; // 稳定轨迹预计争同格时，给路线增加一份等待成本；启发式，非概率。
inline constexpr double opponent_pressure_cost = 0.25; // 下一Tick可能进入稳定轨迹敌车射线时的软成本，不改变攻击时序。
}  // namespace rmtc::match_config

