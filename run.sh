#!/usr/bin/env bash
set -eo pipefail
# 启动已经编译好的选手程序。比赛配置只取match_config.hpp，不在这里补值。
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
# 改过配置或尚未用当前构建脚本构建时，明确停下；不能运行旧模型、旧地址或旧令牌。
if [[ ! -f build/rmtc_bot/match_config.sha256 ]] || ! sha256sum --check --status build/rmtc_bot/match_config.sha256; then
  echo '当前配置尚未构建。请 bash build.sh，停止旧进程，再 bash run.sh。' >&2
  exit 2
fi
rmtc_bot_executable="$PWD/install/rmtc_bot/lib/rmtc_bot/bot" # 只运行本工作空间的二进制，不受其他overlay影响。
if [[ ! -x "$rmtc_bot_executable" ]]; then
  echo '本工作空间没有可执行bot，请先 bash build.sh。' >&2
  exit 1
fi
echo "启动：$rmtc_bot_executable"
mkdir -p "$PWD/log"
rmtc_run_log="$PWD/log/bot_$(date +%Y%m%d_%H%M%S)_$$.log" # 本次A端的完整输出，与裁判run_dir分开保存。
echo "本次机器人日志：$rmtc_run_log"
"$rmtc_bot_executable" 2>&1 | tee "$rmtc_run_log" # pipefail保留机器人失败状态；无需再手工加tee。
