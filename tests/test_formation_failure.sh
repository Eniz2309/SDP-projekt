#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="formation_failure"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

start_drone DRON_001 abc123 120
start_drone DRON_002 abc123 120 SDP_INITIAL_BATTERY=21 SDP_BATTERY_TICK_SECONDS=2
start_drone DRON_003 abc123 120
for d in DRON_001 DRON_002 DRON_003; do
  wait_drone_available "$d" 12 || fail "$d nije AVAILABLE"
done

mission M_FORM_FAIL FORMATION SKENDERIJA SKENDERIJA_K2 120 3 10 >"$RESULT_DIR/formation_submit.json"
wait_mission_status M_FORM_FAIL ACTIVE 8 || fail "Formation nije ACTIVE"
wait_mission_status M_FORM_FAIL ABORTED_FORMATION_LOW_BATTERY 15 || fail "Formation nije abortirana zbog LOW_BATTERY clana"

STATUSES="$(central_sql "SELECT group_concat(drone_uri || ':' || status, ',') FROM (SELECT drone_uri,status FROM formation_members WHERE mission_id='M_FORM_FAIL' ORDER BY drone_uri);")"
[[ "$STATUSES" == *"DRON_002:FAILED_LOW_BATTERY"* ]] || fail "Failed clan nije oznacen FAILED_LOW_BATTERY: $STATUSES"
[[ "$STATUSES" == *"DRON_001:STOPPED"* ]] || fail "DRON_001 nije STOPPED: $STATUSES"
[[ "$STATUSES" == *"DRON_003:STOPPED"* ]] || fail "DRON_003 nije STOPPED: $STATUSES"
pass "Formation failure bilo kojeg clana"
