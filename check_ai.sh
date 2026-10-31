#!/usr/bin/env bash
set -eo pipefail
# 赛前练习诊断：只检查练习代理向真实模型的出网链路，不启动ROS或裁判。
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if (( $# != 0 )); then
  echo '配置只改 src/rmtc_bot/include/rmtc_bot/match_config.hpp，再 bash build.sh。' >&2
  exit 2
fi
if [[ ! -x install/rmtc_bot/lib/rmtc_bot/practice_launcher || ! -f build/rmtc_bot/match_config.sha256 || ! -f build/rmtc_bot/ai_check.sha256 ]]; then
  echo '请先 bash build.sh，再 bash check_ai.sh。' >&2
  exit 2
fi
if ! sha256sum --check --status build/rmtc_bot/match_config.sha256 || ! sha256sum --check --status build/rmtc_bot/ai_check.sha256; then
  echo '配置或诊断入口已改变但尚未构建，请先 bash build.sh。' >&2
  exit 2
fi
exec "$PWD/install/rmtc_bot/lib/rmtc_bot/practice_launcher" --check-ai
