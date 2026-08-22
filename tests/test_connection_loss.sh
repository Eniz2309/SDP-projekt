#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="connection_loss"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

start_drone DRON_CONN abc123 120
wait_drone_available DRON_CONN 12 || fail "DRON_CONN nije AVAILABLE"
mission M_CONN MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
wait_mission_status M_CONN ACTIVE 8 || fail "Connection-loss misija nije ACTIVE"

log "Namjerno gasim regionalni server da simuliram potpuni TCP/TLS prekid."
kill "$REGIONAL_PID"
wait "$REGIONAL_PID" 2>/dev/null || true
REGIONAL_PID=""

wait_log "${DRONE_LOGS[DRON_CONN]}" "TCP/TLS veza sa regionalnim serverom je izgubljena" 8 || fail "Dron nije detektovao connection loss"
wait_log "${DRONE_LOGS[DRON_CONN]}" "Lokalni RTB zavrsen" 8 || fail "Drone-side failsafe RTB nije izvrsen"
assert_contains_file "${DRONE_LOGS[DRON_CONN]}" "AT_BASE_CONNECTION_LOST" "Dron nije zavrsio u AT_BASE_CONNECTION_LOST"
pass "CONNECTION_LOST -> lokalni failsafe RTB"
