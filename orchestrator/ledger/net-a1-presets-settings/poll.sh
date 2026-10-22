#!/bin/bash
# one gentle poller: one request per 12 minutes; exits when all 8 checks are complete or any is not success
W="C:/Method Notes Obsidian/gh-epicnicks.sh"
SHA=8070435a4153d88e3fd17746ff01f8284f30fe6e
cd "C:/Method Notes Obsidian/N2Engine"
sleep 60
for i in $(seq 1 20); do
  R=$(bash "$W" api repos/EpicNicks/N2Engine/commits/$SHA/check-runs --jq '.check_runs[]|"\(.name) \(.status) \(.conclusion)"' 2>&1 | cut -c1-160)
  echo "--- poll $i"; echo "$R"
  if echo "$R" | grep -q "rate limit"; then sleep 900; continue; fi
  N=$(echo "$R" | grep -c " completed ")
  F=$(echo "$R" | grep -c -E " completed (failure|cancelled|timed_out)")
  if [ "$F" -gt 0 ] || { [ "$N" -ge 9 ] && [ "$(echo "$R" | grep -c -E " (in_progress|queued) ")" -eq 0 ]; }; then echo FINISHED; exit 0; fi
  sleep 240
done
echo TIMEOUT
