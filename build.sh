#!/usr/bin/env bash
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source /opt/ros/humble/setup.bash
colcon build --symlink-install --base-paths src --packages-select rmtc_bot \
  --cmake-args -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sfn build/rmtc_bot/compile_commands.json compile_commands.json
