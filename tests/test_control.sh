#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="control"
source "$(dirname "$0")/common.sh"
setup_test
start_stack
start_drone DRON_001 abc123 120
wait_drone_available DRON_001 12 || fail "Dron nije AVAILABLE"

mission M_CTRL MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
wait_mission_status M_CTRL ACTIVE 8 || fail "Kontrolna misija nije ACTIVE"

mission PARAMS DRON_001 150 15 EAST >"$RESULT_DIR/params.json"
wait_sql "SELECT altitude || '|' || speed || '|' || direction FROM drones WHERE drone_uri='DRON_001';" "150|15|EAST" 8 || fail "PARAMS nije primijenjen"
pass "Promjena altitude/speed/direction"

mission STOP M_CTRL >"$RESULT_DIR/stop.json"
wait_mission_status M_CTRL STOPPED 10 || fail "STOP_MISSION nije zavrsen"
wait_drone_available DRON_001 10 || fail "Dron nije AVAILABLE nakon STOP"
pass "STOP_MISSION"
