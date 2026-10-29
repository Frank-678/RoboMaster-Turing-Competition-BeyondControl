#!/usr/bin/env bash
# 启动指定 install 底座的席位二进制（docs/38 联赛自战用）。
# 用法：bash tools/run_seat.sh <install_base> <seat> <logfile>
# 节点名重映射 rmtc_bot_<seat>：双实例同 ROS 域下避免 Node("rmtc_bot") 命名冲突，
# 不动源码、不重建冠军二进制。席位/令牌身份由编译期 match_config.hpp 决定。
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
INSTALL_BASE="${1:?install base required}"
SEAT="${2:?seat required}"
LOGFILE="${3:-log/bot_${SEAT}.log}"
case "$SEAT" in A|B|C|D) ;; *) echo '席位须为 A/B/C/D' >&2; exit 2;; esac
case "$INSTALL_BASE" in
  /*) BASE="$INSTALL_BASE" ;;
  *) BASE="$PWD/$INSTALL_BASE" ;;
esac
if [[ ! -f "$BASE/setup.bash" ]]; then
  echo "install 底座不存在：$BASE/setup.bash" >&2
  exit 1
fi
source /opt/ros/humble/setup.bash
source "$BASE/setup.bash"
BOT="$BASE/rmtc_bot/lib/rmtc_bot/bot"
if [[ ! -x "$BOT" ]]; then
  echo "该底座没有可执行 bot：$BOT" >&2
  exit 1
fi
mkdir -p "$PWD/log"
echo "启动：$BOT（seat=$SEAT，node=rmtc_bot_$SEAT，日志=$LOGFILE）"
exec "$BOT" --ros-args -r "__node:=rmtc_bot_$SEAT" 2>&1 | tee "$LOGFILE"
