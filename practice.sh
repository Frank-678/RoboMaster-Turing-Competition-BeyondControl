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
if [[ ! -f build/rmtc_bot/match_config.sha256 ]] || ! sha256sum --check --status build/rmtc_bot/match_config.sha256; then
  echo '当前配置尚未构建。请 bash build.sh，停止旧裁判，再 bash practice.sh。' >&2
  exit 2
fi
# 和run.sh一样固定到本工作空间，防止启动另一个overlay里的旧练习配置。
exec "$PWD/install/rmtc_bot/lib/rmtc_bot/practice_launcher"
