#pragma once
#include "rmtc_bot/events.hpp"

namespace rmtc {
// 赛前手写策略的可调数字。修改后重新编译并评估，再封存代码和配置；
// 正式运行只读取下面的 constexpr 常量，不从网络/文件热更新，也不在线学习。
// 这些数值影响“更想选哪个动作”，不改变动作合法性、官方伤害、计分和事件时序。
// 先只集中少量已有参数，默认值保持原策略行为；本文件不表示已经完成自动调参。
struct StrategyTuning {
  // 资源在视野外可能已被拿走：旧观测的收益乘 1/(1 + 过期Tick数 * 此值)。
  double stale_resource_decay = 0.01;
  // 尚未完成普通/高价值资源任务时，给匹配资源增加的启发式收益，不是官方加分。
  double resource_mission_bonus = 5.0;
  // reach 任务目标的启发式收益，之后还会除以“距离+1”。
  double reach_mission_value = 8.0;
  // HP 低于此值才主动追求治疗点；最大生命等规则数值不在此处调整。
  double healing_below_hp = 70.0;
  // 已在堡垒内时保留驻留的价值，以及前往堡垒的价值分子。
  // 后者仍须除以路程和驻留时间；两者都不是“堡垒必胜”的保证。
  double fortress_hold_value = 20.0;
  double fortress_capture_value = 12.0;
  // 离危险生效还剩“最短退出步数 + 此提前量”时开始撤离，单位为Tick。
  Tick hazard_lead_ticks = 2;
};

inline constexpr StrategyTuning strategy_tuning{};
}  // namespace rmtc
