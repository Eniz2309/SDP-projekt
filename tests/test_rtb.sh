#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="rtb"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

# Koristimo JEDAN dron za oba RTB scenarija. Time scheduler nema izbor
# izmedju vise AVAILABLE dronova i low-battery misija deterministicki
# mora biti dodijeljena upravo dronu ciju bateriju simuliramo.
start_drone DRON_001 abc123 120 SDP_INITIAL_BATTERY=21 SDP_BATTERY_TICK_SECONDS=1
wait_drone_available DRON_001 12 || fail "DRON_001 nije AVAILABLE prije low-battery testa"

mission M_LOW_BAT MONITORING SKENDERIJA SKENDERIJA_K2 120 >"$RESULT_DIR/low_battery_mission.json"
wait_mission_status M_LOW_BAT ACTIVE 8 || fail "Low-battery misija nije ACTIVE"

ASSIGNED_DRONE="$(central_sql "SELECT drone_uri FROM missions WHERE mission_id='M_LOW_BAT';")"
[[ "$ASSIGNED_DRONE" == "DRON_001" ]] || fail "M_LOW_BAT je dodijeljena pogresnom dronu: $ASSIGNED_DRONE"

wait_mission_status M_LOW_BAT ABORTED_LOW_BATTERY 12 || {
  echo "--- DRON_001 zadnjih 80 linija ---" >&2
  tail -80 "${DRONE_LOGS[DRON_001]}" >&2 || true
  echo "--- CENTRAL zadnjih 80 linija ---" >&2
  tail -80 "$CENTRAL_LOG" >&2 || true
  fail "Misija nije ABORTED_LOW_BATTERY"
}

wait_sql_nonzero "SELECT COUNT(*) FROM alarms WHERE drone_uri='DRON_001' AND alarm_type='LOW_BATTERY';" 8 || fail "LOW_BATTERY alarm nije upisan"
wait_log "${DRONE_LOGS[DRON_001]}" "CHARGING started" 8 || fail "Charging nije pokrenut"
wait_drone_available DRON_001 25 || fail "DRON_001 se nakon charging-a nije vratio u AVAILABLE"

BAT="$(central_sql "SELECT battery FROM drones WHERE drone_uri='DRON_001';")"
(( BAT >= 80 )) || fail "Dron je AVAILABLE sa baterijom ispod 80%"
pass "LOW_BATTERY -> RTB -> CHARGING -> AVAILABLE"

# Nakon sto je isti dron napunjen i ponovo AVAILABLE, provjeravamo i
# operatorski/manualni RTB. Nema drugog drona koji bi uticao na scheduler.
mission RTB DRON_001 >"$RESULT_DIR/manual_rtb.json"
wait_log "${DRONE_LOGS[DRON_001]}" "Returning to base because of MANUAL_OPERATOR_REQUEST" 8 || fail "Manualni RTB nije izvrsen na dronu"
wait_drone_available DRON_001 12 || fail "DRON_001 se poslije manualnog RTB-a nije vratio u AVAILABLE"
pass "Manual RTB"
