#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rmtc {
// RULES v1.2 §10.2: cap the score, never the official cumulative R/D.
// A negative change also supports evaluating death after contributions/rewards.
inline double score_delta(double total, double change, double cap, double maximum) {
  if (!std::isfinite(total) || total < 0 || !std::isfinite(change) ||
      !std::isfinite(cap) || cap <= 0 || !std::isfinite(maximum) || maximum < 0)
    throw std::runtime_error("invalid scoring input");
  return maximum * (std::clamp(total + change, 0.0, cap) - std::clamp(total, 0.0, cap)) / cap;
}
}  // namespace rmtc
