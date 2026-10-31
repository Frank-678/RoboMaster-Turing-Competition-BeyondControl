#!/usr/bin/env bash
# 两阶段切换构建：bash tools/build_stage.sh 1|2
# 把 match_config.hpp 的编译期 stage 改为给定值并重建。
# 正式比赛：每个阶段开始前确认代码版本；第一阶段后的统一调整窗口内用本脚本切回第二阶段。
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
S="${1:?用法: bash tools/build_stage.sh 1|2}"
case "$S" in 1|2) ;; *) echo "stage 只能是 1 或 2" >&2; exit 2 ;; esac
M=src/rmtc_bot/include/rmtc_bot/match_config.hpp
if ! grep -q "inline constexpr int stage = $S;" "$M"; then
  sed -i "s/inline constexpr int stage = [12];/inline constexpr int stage = $S;/" "$M"
fi
bash build.sh
grep "inline constexpr int stage = " "$M"
