#!/bin/bash
# poll connections on the OTA server port once a second (s_server -WWW writes no access log)
while true; do
  out=$(ss -tni '( sport = :8070 )' | grep -v '^State')
  if [ -n "$out" ]; then echo "=== $(date +%H:%M:%S)"; echo "$out"; fi
  sleep 1
done
