#!/usr/bin/env bash
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -f install/setup.bash ]]; then
  echo '请先在 ~/cst 中运行 bash build.sh' >&2
  exit 1
fi
source /opt/ros/humble/setup.bash
source install/setup.bash
cst_seat="${1:-A}"
cst_name="${2:-Frank}"
case "$cst_seat" in A|B|C|D) ;; *) echo '席位须为 A/B/C/D' >&2; exit 2 ;; esac
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-27}"
export ROS_LOCALHOST_ONLY="${ROS_LOCALHOST_ONLY:-0}"
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
exec ros2 run rmtc_bot bot --ros-args -p "team:=$cst_seat" -p "team_name:=$cst_name"
