#!/bin/bash
cd /home/frank/cst
echo "== git status =="
git status --short
echo "== last commit =="
git log --oneline -1
echo "== key fields in match_config.hpp =="
grep -n "practice_llm_key\|practice_llm_proxy" src/rmtc_bot/include/rmtc_bot/match_config.hpp
echo "== build stamp =="
ls -la build/rmtc_bot/match_config.sha256 2>/dev/null && sha256sum --check --status build/rmtc_bot/match_config.sha256 2>/dev/null && echo "STAMP OK" || echo "STAMP BROKEN"
