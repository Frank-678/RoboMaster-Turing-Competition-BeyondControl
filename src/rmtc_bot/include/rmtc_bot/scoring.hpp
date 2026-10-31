#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rmtc {
// 官方计分：maximum * min(total/cap,1)。比较改变前后分差，别把原始R/D截断。
// 例如D=600时死一次降到540，对抗分仍满；D=520降到460则会掉分。
// 本函数只估算分差；真实累计值始终读取最新感知，不自行重复加击杀奖励或扣死亡。
// total当前R/D，change预计变化，cap封顶门槛，maximum该计分项满分。
inline double score_delta(double total, double change, double cap, double maximum) {
  if (!std::isfinite(total) || total < 0 || !std::isfinite(change) ||
      !std::isfinite(cap) || cap <= 0 || !std::isfinite(maximum) || maximum < 0)
    throw std::runtime_error("invalid scoring input");
  return maximum * (std::clamp(total + change, 0.0, cap) - std::clamp(total, 0.0, cap)) / cap;
}
}  // namespace rmtc
