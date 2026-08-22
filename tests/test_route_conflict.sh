#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="route_conflict"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

for d in DRON_001 DRON_002 DRON_003 DRON_004; do
  start_drone "$d" abc123 120
  wait_drone_available "$d" 12 || fail "$d nije AVAILABLE"
done

mission M_R1 MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
mission M_R2 MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
mission M_R3 MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null

for m in M_R1 M_R2 M_R3; do
  wait_mission_status "$m" ACTIVE 10 || fail "$m nije ACTIVE"
done

ALTITUDES="$(central_sql "SELECT group_concat(altitude, ',') FROM (SELECT altitude FROM missions WHERE mission_id IN ('M_R1','M_R2','M_R3') ORDER BY altitude);")"
assert_eq "$ALTITUDES" "120,122,124" "Altitude slotovi nisu 120/122/124"
pass "Vertikalni slotovi"

mission M_CONFLICT DELIVERY SKENDERIJA AUTO 120 43.8563 18.42057 >"$RESULT_DIR/conflict_submit.json"
wait_mission_status M_CONFLICT QUEUED 8 || fail "Konfliktna DELIVERY nije ostala QUEUED"
assert_sql_eq "SELECT queue_reason FROM missions WHERE mission_id='M_CONFLICT';" "ROUTE_CONFLICT_NO_SAFE_ALTITUDE" "Nedostaje ispravan queue_reason"
pass "Geometrijski route conflict -> QUEUED"

TARGET_DRONE="$(central_sql "SELECT drone_uri FROM drones WHERE mission_id IN ('M_R1','M_R2','M_R3') AND altitude=122 LIMIT 1;")"
[[ -n "$TARGET_DRONE" ]] || fail "Nije pronadjen dron na 122 m za PARAMS conflict test"
PARAMS_OUT="$RESULT_DIR/params_conflict.json"
mission PARAMS "$TARGET_DRONE" 120 15 EAST >"$PARAMS_OUT" || true
assert_contains_file "$PARAMS_OUT" "ROUTE_CONFLICT_AT_REQUESTED_ALTITUDE" "PARAMS nije odbio konflikt visine"
pass "PARAMS route-safety"
