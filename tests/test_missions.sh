#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="missions"
source "$(dirname "$0")/common.sh"
setup_test
start_stack
start_drone DRON_001 abc123 120
wait_drone_available DRON_001 12 || fail "Dron nije AVAILABLE"

mission M_MON_TEST MONITORING SKENDERIJA SKENDERIJA_K1 120 >"$RESULT_DIR/monitoring_submit.json"
wait_mission_status M_MON_TEST ACTIVE 8 || fail "MONITORING nije aktiviran"
assert_sql_eq "SELECT mission_type FROM drones WHERE drone_uri='DRON_001';" "MONITORING" "Dron nema MONITORING task"
mission STOP M_MON_TEST >/dev/null
wait_mission_status M_MON_TEST STOPPED 10 || fail "MONITORING STOP nije zavrsen"
wait_drone_available DRON_001 10 || fail "Dron nije vracen u AVAILABLE"
pass "MONITORING + STOP"

mission M_DEL_TEST DELIVERY SKENDERIJA AUTO 120 43.8580 18.4160 >"$RESULT_DIR/delivery_submit.json"
wait_sql_nonzero "SELECT COUNT(*) FROM missions WHERE mission_id='M_DEL_TEST' AND status IN ('ACTIVE','FINISHED');" 8 || fail "DELIVERY nije dodijeljen"
assert_sql_eq "SELECT mission_type FROM missions WHERE mission_id='M_DEL_TEST';" "DELIVERY" "Pogresan DELIVERY tip"
stop_mission_if_active M_DEL_TEST
wait_drone_available DRON_001 12 || fail "Dron nije raspoloziv nakon DELIVERY"
pass "DELIVERY"

mission M_INS_TEST INSPECTION SKENDERIJA SKENDERIJA_K2 120 >"$RESULT_DIR/inspection_submit.json"
wait_sql_nonzero "SELECT COUNT(*) FROM missions WHERE mission_id='M_INS_TEST' AND status IN ('ACTIVE','FINISHED');" 8 || fail "INSPECTION nije dodijeljen"
assert_sql_eq "SELECT mission_type FROM missions WHERE mission_id='M_INS_TEST';" "INSPECTION" "Pogresan INSPECTION tip"
stop_mission_if_active M_INS_TEST
wait_drone_available DRON_001 12 || fail "Dron nije raspoloziv nakon INSPECTION"
pass "INSPECTION"

mission M_TEST_FLIGHT TEST_FLIGHT SKENDERIJA SKENDERIJA_K3 120 >"$RESULT_DIR/testflight_submit.json"
wait_sql_nonzero "SELECT COUNT(*) FROM missions WHERE mission_id='M_TEST_FLIGHT' AND status IN ('ACTIVE','FINISHED');" 8 || fail "TEST_FLIGHT nije dodijeljen"
assert_sql_eq "SELECT mission_type FROM missions WHERE mission_id='M_TEST_FLIGHT';" "TEST_FLIGHT" "Pogresan TEST_FLIGHT tip"
stop_mission_if_active M_TEST_FLIGHT
pass "TEST_FLIGHT"
