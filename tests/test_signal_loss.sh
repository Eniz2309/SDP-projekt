#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="signal_loss"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

start_drone DRON_SIGNAL abc123 120 \
  SDP_INITIAL_SIGNAL=25 SDP_SIGNAL_TICK_SECONDS=2 SDP_SIGNAL_DROP_PER_TICK=3
wait_drone_available DRON_SIGNAL 12 || fail "DRON_SIGNAL nije AVAILABLE"
mission M_SIGNAL MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
wait_mission_status M_SIGNAL ACTIVE 8 || fail "Signal test misija nije ACTIVE"
wait_mission_status M_SIGNAL ABORTED_SIGNAL_LOSS 12 || fail "Misija nije ABORTED_SIGNAL_LOSS"
wait_sql_nonzero "SELECT COUNT(*) FROM alarms WHERE drone_uri='DRON_SIGNAL' AND alarm_type='SIGNAL_LOSS';" 8 || fail "SIGNAL_LOSS alarm nije upisan"
wait_drone_available DRON_SIGNAL 15 || fail "Dron se nakon SIGNAL_LOSS RTB-a nije vratio u AVAILABLE"
pass "SIGNAL_LOSS -> alarm -> RTB"
