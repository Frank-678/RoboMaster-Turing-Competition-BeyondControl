#pragma once

// 比赛前只改这里，然后 bash build.sh。以下都是明确配置，不读环境变量、不猜缺失值。
namespace rmtc::match_config {
inline constexpr const char *team = "A";       // 裁判分配的本场席位。
inline constexpr const char *team_name = "Frank";
inline constexpr int stage = 2;               // 1：采集；2：对抗。
inline constexpr int stage1_ticks = 100;       // 正式规则§3：动作0–99，100是最终状态。
inline constexpr int stage2_ticks = 500;       // 正式规则§3：动作0–499，500是最终状态；改练习长度时在这里明确改。

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
inline constexpr double goal_retention_bonus = 0.10; // 同一有效目标给10%排序余量，抑制小波动；不是官方加分。
inline constexpr int resource_plan_depth = 3;       // 资源串最多扩展3个目标，不是完整战斗树或3Tick。
inline constexpr int resource_beam_width = 5;       // 每层最多保留5条未执行计划，优先保留不同首方向。
inline constexpr int resource_candidate_limit = 8;  // 先按现有收益/距离挑8个资源；地图资源数不写死。
inline constexpr int resource_plan_budget_ms = 10;  // 额外搜索软预算；耗尽时交回已算好的合法基线。
inline constexpr double frontier_value = 0.02;      // 每份潜在新视野的信息权重；不是资源分或必然有资源的概率。
inline constexpr double opponent_contest_cost = 1.0; // 稳定轨迹预计争同格时，给路线增加一份等待成本；启发式，非概率。
inline constexpr double opponent_pressure_cost = 0.25; // 下一Tick可能进入稳定轨迹敌车射线时的软成本，不改变攻击时序。
inline constexpr bool collision_replan = true; // 同一起点同一落点连续两次MOVE失败，存在合法替代时改道一Tick。
inline constexpr bool resource_refinement = true;  // 重评上一资源串，并尝试插入、交换/反转、删除目标。
inline constexpr int resource_refinement_trials = 12; // 每帧最多12次局部修改，共享原有10ms软预算。
inline constexpr bool tactical_search = true;       // 可见敌车附近才比较最多三Tick、多个合法对手情景。
inline constexpr double tactical_risk = 0.35;        // 最差情景的权重；不是经训练的概率。
inline constexpr double tactical_hp_value = 0.015;   // 每HP的未来机会估值，不写入官方分数。
inline constexpr double tactical_death_cost = 1.0;   // 除规则D扣除外，死亡丢失位置/行动窗口的估值。
inline constexpr double tactical_future_discount = 0.65; // 第二Tick相对第一Tick的收益权重。
inline constexpr double tactical_goal_weight = 0.35; // 原目标评价在战术比较中的尾部价值。
}  // namespace rmtc::match_config

