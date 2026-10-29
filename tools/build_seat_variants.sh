#!/usr/bin/env bash
# 席位变体在独立源码副本里构建；失败/中断也不会改当前队伍配置或覆盖冠军安装。
# bash tools/build_seat_variants.sh SOURCE_DIR VERSION A,B [--manifest tools/versions.json]
set -eo pipefail
rmtc_tool_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source /opt/ros/humble/setup.bash
exec python3 "$rmtc_tool_dir/build_seat_variants.py" "$@"
