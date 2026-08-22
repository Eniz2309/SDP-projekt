#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="rtb"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

start_drone DRON_001 abc123 120
wait_drone_available DRON_001 12 || fail "DRON_001 nije AVAILABLE"
mission RTB DRON_001 >"$RESULT_DIR/manual_rtb.json"
wait_log "${DRONE_LOGS[DRON_001]}" "Returning to base because of MANUAL_OPERATOR_REQUEST" 8 || fail "Manualni RTB nije izvrsen na dronu"
wait_drone_available DRON_001 12 || fail "DRON_001 se nije vratio u AVAILABLE"
pass "Manual RTB"

start_drone DRON_002 abc123 120 SDP_INITIAL_BATTERY=21 SDP_BATTERY_TICK_SECONDS=2
wait_drone_available DRON_002 12 || fail "DRON_002 nije AVAILABLE prije low-battery testa"
mission M_LOW_BAT MONITORING SKENDERIJA SKENDERIJA_K2 120 >/dev/null
wait_mission_status M_LOW_BAT ACTIVE 8 || fail "Low-battery misija nije ACTIVE"
wait_mission_status M_LOW_BAT ABORTED_LOW_BATTERY 12 || fail "Misija nije ABORTED_LOW_BATTERY"
wait_sql_nonzero "SELECT COUNT(*) FROM alarms WHERE drone_uri='DRON_002' AND alarm_type='LOW_BATTERY';" 8 || fail "LOW_BATTERY alarm nije upisan"
wait_drone_available DRON_002 25 || fail "DRON_002 se nakon charging-a nije vratio u AVAILABLE"
BAT="$(central_sql "SELECT battery FROM drones WHERE drone_uri='DRON_002';")"
(( BAT >= 80 )) || fail "Dron je AVAILABLE sa baterijom ispod 80%"
pass "LOW_BATTERY -> RTB -> CHARGING -> AVAILABLE"
