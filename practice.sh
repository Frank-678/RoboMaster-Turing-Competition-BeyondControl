#!/usr/bin/env bash
set -eo pipefail
# 运行我们自己的配置启动器；被调用的practice/start_referee.sh始终保持原样。
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if (( $# != 0 )); then
  echo '所有配置写在 src/rmtc_bot/include/rmtc_bot/match_config.hpp，启动脚本不收参数。' >&2
  exit 2
fi
if [[ ! -f install/setup.bash || ! -f practice/start_referee.sh ]]; then
  echo '需要原版practice/，并先 bash build.sh。' >&2
  exit 1
fi
source /opt/ros/humble/setup.bash
source install/setup.bash
exec ros2 run rmtc_bot practice_launcher
