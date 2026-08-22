#!/usr/bin/env bash
set -uo pipefail
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS="$TESTS_DIR/results/test_results.txt"
mkdir -p "$TESTS_DIR/results"
: >"$RESULTS"

TESTS=(
  test_auth.sh
  test_missions.sh
  test_priority.sh
  test_control.sh
  test_route_conflict.sh
  test_rtb.sh
  test_formation_failure.sh
  test_signal_loss.sh
  test_connection_loss.sh
  test_udp_replay.sh
)

pass_count=0
fail_count=0
skip_count=0

for t in "${TESTS[@]}"; do
  echo "============================================================"
  echo "RUN $t"
  echo "============================================================"
  "$TESTS_DIR/$t"
  rc=$?
  if [[ $rc -eq 0 ]]; then
    echo "[PASS] $t" | tee -a "$RESULTS"
    ((pass_count++))
  elif [[ $rc -eq 77 ]]; then
    echo "[SKIP] $t" | tee -a "$RESULTS"
    ((skip_count++))
  else
    echo "[FAIL] $t" | tee -a "$RESULTS"
    ((fail_count++))
  fi
  sleep 0.5
done

{
  echo
  echo "TOTAL PASS=$pass_count FAIL=$fail_count SKIP=$skip_count"
} | tee -a "$RESULTS"

[[ $fail_count -eq 0 ]]
