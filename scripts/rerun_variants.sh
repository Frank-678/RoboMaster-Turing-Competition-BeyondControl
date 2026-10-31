#!/bin/bash
# 重跑批量 variants 4-7（--only 会消费0..i-1的RNG流以复现原批次变体）。
cd /home/frank
for i in 4 5 6 7; do
  echo "=== rerun variant $i at $(date +%H:%M:%S)"
  python3 -I /home/frank/batch_practice.py --count 8 --seed 20261029 --only "$i" --summary "/home/frank/batch_summary_$i.json"
  echo "=== variant $i exit $?"
done
echo ALL_RERUNS_DONE
