#!/usr/bin/env bash
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if (( $# != 0 )); then
  echo '所有配置写在 src/rmtc_bot/include/rmtc_bot/match_config.hpp；修改后构建，再 bash run.sh' >&2
  exit 2
fi
if [[ ! -f install/setup.bash ]]; then
  echo '请先在 ~/cst 中运行 bash build.sh' >&2
  exit 1
fi
source /opt/ros/humble/setup.bash
source install/setup.bash
# 席位、阶段、AI和ROS网络参数全部由编译进程序的match_config.hpp提供。
exec ros2 run rmtc_bot bot
