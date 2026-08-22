#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="auth"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

start_drone DRON_001 abc123 120
wait_drone_available DRON_001 12 || fail "Validan URI+TOKEN nije postao AVAILABLE"
assert_sql_eq "SELECT enabled FROM drone_credentials WHERE drone_uri='DRON_001';" "1" "Credential nije aktivan"
pass "Validan URI+TOKEN"

INVALID_LOG="$RESULT_DIR/invalid_token.log"
run_drone_timed DRON_002 POGRESAN_TOKEN "$INVALID_LOG" 5 || true
assert_contains_file "$INVALID_LOG" "INVALID_TOKEN" "Pogresan token nije odbijen"
pass "INVALID_TOKEN"

UNKNOWN_LOG="$RESULT_DIR/unknown_uri.log"
run_drone_timed DRON_HACKER abc123 "$UNKNOWN_LOG" 5 || true
assert_contains_file "$UNKNOWN_LOG" "UNKNOWN_DRONE_URI" "Nepoznat URI nije odbijen"
pass "UNKNOWN_DRONE_URI"
