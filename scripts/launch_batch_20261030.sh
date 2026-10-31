#!/bin/bash
set -u
cd /home/frank/cst
python3 -I /home/frank/batch_practice.py --count 8 --seed 20261030 --summary /home/frank/batch_summary_20261030.json
echo "BATCH_DONE exit=$?"
