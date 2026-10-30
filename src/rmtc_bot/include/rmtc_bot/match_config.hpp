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
inline constexpr int ai_max_tokens = 2200;    // 多事件JSON预算；公告示例512不是已确认的赛事硬上限，按实际配额修改。
inline constexpr const char *ai_http_proxy = ""; // 机器人→网关的网络代理；空串明确表示直连。
// 2026-10-28正式公告：统一deepseek-flash，其他字段直接转发，默认思考开启。
// 必须在请求JSON顶层明确disabled；不是改URL，也不是只改练习裁判配置。
inline constexpr const char *ai_thinking = "disabled";
inline constexpr bool ai_json_output = true; // 明确请求JSON模式；Prompt也要求JSON，仍需本地校验。

// 只供 bash practice.sh 使用：练习代理→真实模型。正式赛不使用这组上游配置。
// 模型名共用上面的ai_model。真实服务密钥与practice-A是两种不同的凭据。
inline constexpr int practice_port = 8080;     // 原版练习代理监听端口，须与本机ai_url一致。
inline constexpr const char *practice_llm_url = "https://api.deepseek.com/chat/completions"; // DeepSeek文档给出的完整POST端点。
inline constexpr const char *practice_llm_key = "sk-cbc672872863418b9d70cdc59686e5c4"; // 用户授权写死 2026-10-30（源头将切断），可入库提交。
inline constexpr const char *practice_llm_proxy = "http://127.0.0.1:7890"; // 用户授权写死 2026-10-30 WSL出网代理。

// 下面只调策略取舍。攻击距离、伤害、计分公式和结算顺序按规则实现，不在这里调。
inline constexpr double stale_resource_decay = 0.01; // 资源记忆每Tick衰减系数。
inline constexpr double resource_mission_bonus = 5.0; // 未完成采集任务时的额外权重。
inline constexpr double reach_mission_value = 8.0;    // 到点任务权重，还会除以距离+1。
inline constexpr double healing_below_hp = 40.0;     // 低于此血量才主动找治疗点。
inline constexpr double fortress_hold_value = 20.0;  // 保住尚未完成的驻留进度。
inline constexpr double fortress_capture_value = 12.0; // 前往堡垒的收益分子。
inline constexpr double prepare_fortress_value = 0.0; // 堡垒预告提前占点（自visible-resource-bfs采纳的行为杠杆，默认关）。家族已关闭：fortress轮（levers-fortress-2026-10-29）pf03/06/12/24 objective 0.7971→0.7889（各剂量持平为负），deaths 83→87-92，win_share 0.333→0.25；vrb 原版行为（直接用 fortress_capture_value=12.0）≡pf12，可复现地更差。
inline constexpr int hazard_lead_ticks = 2;          // 比最短退出时间再提前几Tick。
inline constexpr double goal_retention_bonus = 0.0; // 同一有效目标的排序余量；调参未发现正向作用，关闭。
inline constexpr int stale_grace_ticks = 0; // 资源离开视野后前N个Tick不衰减（未观测≠消失），压目标震荡；0=离视野当Tick即衰减。默认关，待竞技场评估晋升。
inline constexpr int resource_plan_depth = 4;       // 资源串最多扩展4个目标，不是完整战斗树或3Tick。
inline constexpr int resource_beam_width = 5;       // 每层最多保留5条未执行计划，优先保留不同首方向。
inline constexpr int resource_candidate_limit = 8;  // 先按现有收益/距离挑8个资源；地图资源数不写死。
inline constexpr int resource_plan_budget_ms = 10;  // 额外搜索软预算；耗尽时交回已算好的合法基线。
inline constexpr double frontier_value = 0.02;      // 第二阶段每份潜在新视野的信息权重；不是资源分或必然有资源的概率。第二阶段扫图与任务/交战公平竞争。
inline constexpr double stage1_frontier_value = 0.02; // 第一阶段边界信息权重；与阶段二分离——arena-frontier-2026-10-29轮证实探索权重对两阶段影响相反（阶段二受益/阶段一受害），共享会互相绑架。
inline constexpr double explore_toward_resource_value = 0.5; // 已知资源绕行过远/未连通时，向它试探边界格的机会权重。只影响排序，不把未知格当空地；gate-frontier调参在组合中验证为正。
inline constexpr double mission_explore_value = 0.0; // 沿到点任务方向探路的权重（除以距离+1）；到点任务没有截止时间，拉拽会在资源比赛早期拖离采集线，关闭。
inline constexpr double explore_tour_value = 0.03; // 第一阶段空档期，把散布的边界观察点排成小巡回的机会权重；0为关闭。
inline constexpr double stage1_flood_value = 0.0; // 第一阶段空档探索的最近边界洪泛权重（距离优先、同距离取更大新视野）；与巡回/frontier同场竞争，0为关闭。
inline constexpr double stage1_ring_value = 0.0; // 第一阶段“近出生点完成覆盖”权重：巡回观察点按增益+近出生点加成排序，优先揭示距出生点近的未探索格（rooms-195左侧列30R口袋滞留t91-99的修复轴）；0为关闭。
inline constexpr bool stage2_race_gate = false; // 第二阶段竞速闸门：两轮实验均显示早期主动交火有压制对手扫资源的价值，关闭。
inline constexpr double race_reach_holdoff = 0.0; // 第二阶段竞速期到点任务暂缓折扣（0-1）：docs/35 损失路径=首分钟资源争抢——已知可达资源未清空时 reach_mission 按 (1-holdoff) 打折（V1 t62 弃 [7,4]+[6,2] 20R、V4 t14 弃 10R），资源清空后恢复原权重（M 点晚拿不丢）。与 stage2_race_gate 不同：不动交火轴（gate 两轮实验已证早期交火有压制对手扫资源的价值）。0=旧行为。
inline constexpr int stage2_explore_gate_tick = 0; // 第二阶段资源饼通常在150Tick内被抢光：Tick≥此值且已知资源已清空时关闭扫图，让位给交火/据点；0为关闭。
inline constexpr int stage2_mission_pull_stale_ticks = 0; // 第二阶段资源久未现身（前沿扫图空转）时向到点任务方向拉拽探索；0为关闭。
inline constexpr double stage2_assassin_value = 0.0; // 第二阶段派窗口刺杀：攻击正在扫资源簇的对手，打断其扫掠（击杀/击退→重生移位）把簇剩余资源留给我们；0为关闭。
inline constexpr double stage2_pie_attack_discount = 0.0; // 第二阶段饼窗口攻击估值折扣：竞速期攻击会把我们从抢饼线路吸走（clusters-495：t20-26攻击致死、错过中簇70R）；还手不折扣。0=关闭，1=饼窗内除还手外不主动攻击。与race gate不同：只压攻击估值，不动任务/接近行为。
inline constexpr double stage2_shadow_value = 0.0; // 第二阶段饼窗口影子跟踪：对手连续同向穿过我们已知区朝未知区移动时，向其投影前端探路，与对手在饼簇两侧合流共享；0为关闭。不假设未知格里有资源，只利用对手行为蕴含的信息。
inline constexpr int defend_stall_break_ticks = 0; // 第二阶段连续致命防御驻守超过该tick数后脱离接触（选距最近敌车最远的安全邻格）；0为关闭。
inline constexpr double opponent_contest_cost = 1.0; // 稳定轨迹预计争同格时，给路线增加一份等待成本；启发式，非概率。
inline constexpr double opponent_pressure_cost = 0.25; // 下一Tick可能进入稳定轨迹敌车射线时的软成本，不改变攻击时序。
inline constexpr double opponent_claim_discount = 0.5; // 敌车直线距离不比自己绕障距离长时的资源估值乘数；小于1.0则避开会被先拿走的资源。
inline constexpr bool loop_recovery = true; // 真实位置在小区域反复、且没有实际进展时脱离循环。
inline constexpr int loop_window_ticks = 12; // 至少12份连续可信回执；能识别周期1至6及四格内徘徊。
inline constexpr int recovery_ticks = 12; // 脱困目标最多坚持12Tick，每帧仍检查道路和危险。
inline constexpr int stalled_goal_cooldown_ticks = 24; // 暂缓刚导致循环的目标，过期后可重新竞争。
inline constexpr bool collision_replan = true; // 同一起点同一落点连续两次MOVE失败，存在合法替代时改道一Tick。
inline constexpr bool resource_refinement = true;  // 重评上一资源串，并尝试插入、交换/反转、删除目标。
inline constexpr int resource_refinement_trials = 12; // 每帧最多12次局部修改，共享原有10ms软预算。
inline constexpr bool tactical_search = false;       // 保留三Tick搜索供赛前试验；本轮训练选中关闭，不假定更复杂必然更强。
inline constexpr double tactical_risk = 0.6;        // 最差情景的权重；不是经训练的概率。
inline constexpr double tactical_hp_value = 0.015;   // 每HP的未来机会估值，不写入官方分数。
inline constexpr double tactical_death_cost = 0.0;   // 除规则D扣除外，死亡丢失位置/行动窗口的估值；调参未发现正向作用。
inline constexpr double tactical_future_discount = 0.65; // 第二Tick相对第一Tick的收益权重。
inline constexpr double tactical_goal_weight = 0.65; // 原目标评价在战术比较中的尾部价值。
inline constexpr bool worst_case_survival = false; // 1=生存判定按全敌攻击最坏情形（预测防御只作机会估值，不作伤害上界的确定排除项）；0=旧行为。家族已关闭：survival轮（levers-survival-2026-10-29）objective 0.7971→0.7597，deaths 62↓但blocked 93→114、stage2 41.25→37.84、D 1026.7→677.1、win_share 0.333→0.25——过度求存把机器人压成挨打龟缩。
inline constexpr double low_hp_attack_discount = 1.0; // 低血量攻击折扣：hp<60且未被火力波及时攻击估值乘此值（策略层§6+战术层深度0攻击分支同口径）。依据docs/33 T5：53死亡中40例最后动作为攻击且hp<60、12例死亡tick存在一步安全方向、8例可撤退（B0t388/B1t126/B2t68,347,419,491/B4t285/B5t335）；1.0=旧行为（冠军逐位一致）。
inline constexpr double explore_component_value = 0; // 第二阶段扫图观察点按视野包络触及的最大未探索连通区大小额外加权（覆盖最大化探索方向，大空白区优先于近处零碎口袋）；家族已关闭：第一阶段全负（coverage轮score 33.33→30.3~31.2）、第二阶段train cov008 objective +0.0113（无排名翻转，win持平）但heldout一次性验证 vs 冠军封存0.8299（arena-postcap-heldout-2026-10-29，同30场景，冻结基线0.7710073逐位复现）为0.7906即**−0.039**，stage2 score 42.06<43.875、R增益未复现；0.002持平、0.02崩（R 50.8）。0=旧行为，永久关闭。
inline constexpr double post_cap_combat_weight = 0.02; // D≥500封顶后增量D的平局估值权重（同分同任务同R按D决胜）。train晋升：objective 0.7891→0.8056（win_share 0.25→0.417，rooms-s401/crossroads-s542 rk2→rk1，deaths 56→72），0.05/0.1无更多增益；heldout一次性验证通过：objective 0.771→0.830（vs冻结baseline），stage2 score 40.8→43.9、R 61.3→69.6、win_share 0.375→0.458、invalid 0（arena-postcap-heldout-2026-10-29）。
inline constexpr double explore_probe_mission_bonus_scale = 1.0; // 资源探针（explore_toward_resource）的采集任务加成缩放：探针只是向绕路过远/未连通资源试探边界格，不保证拿到资源，按采集全额计入任务加成会沿资源走廊拖走行动力（R2 snake-A-pressure T22：探针右拉压过 frontier 下探接敌）。1.0=旧行为；0=探针只按封顶分差估值，竞技场评估后再定值。
}  // namespace rmtc::match_config

