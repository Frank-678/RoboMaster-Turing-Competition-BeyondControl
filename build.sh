#!/usr/bin/env bash
set -eo pipefail
# 固定到工程根目录，避免在src子目录构建出另一个工作空间。
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source /opt/ros/humble/setup.bash
# 只构建选手包；Debug便于定位，编译数据库供编辑器跳转使用。
colcon build --symlink-install --base-paths src --packages-select rmtc_bot \
  --cmake-args -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sfn build/rmtc_bot/compile_commands.json compile_commands.json
