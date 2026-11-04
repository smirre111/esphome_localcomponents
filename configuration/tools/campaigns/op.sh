#!/bin/bash
# op.sh <node-name> <arg> ; waits for ack
cd /c/Development/esphome/configuration
python tools/benchlog.py --note "cover $1 $2" --dir "$(ls -d logs/*_review_rollout4 | tail -1)"
python tools/hubctl.py cover "$1" "$2"
cd /c/Users/reinh/AppData/Local/Temp/claude/c--Development-esphome-configuration/a874eec8-286e-4e68-912d-dcceed62ddad/scratchpad/r4
python w.py "Tracked op acknowledged|Command failed|ACK_ERROR" 40 hub
