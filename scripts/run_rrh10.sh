#!/bin/bash
# rrh10 battle 3-6 resume: holdoff=1.0 binary in place (match_config.hpp:61 uncommitted)
LOG=/home/frank/batch_logs/rrh10_resume_20261029.log
exec >> "$LOG" 2>&1
for v in 3 4 5 6; do
  echo "=== battle $v start $(date +%H:%M:%S) ==="
  python3 /home/frank/batch_practice.py --count 7 --seed 20261103 --only "$v" || { echo "battle $v FAILED"; exit 1; }
  echo "=== battle $v done $(date +%H:%M:%S) ==="
done
echo ALL_DONE
