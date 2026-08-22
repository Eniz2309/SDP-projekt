#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="priority"
source "$(dirname "$0")/common.sh"
setup_test
start_stack
start_drone DRON_001 abc123 120
wait_drone_available DRON_001 12 || fail "Dron nije AVAILABLE"

mission M_BLOCK MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
wait_mission_status M_BLOCK ACTIVE 8 || fail "Blocker misija nije ACTIVE"

mission M_LOW TEST_FLIGHT SKENDERIJA SKENDERIJA_K2 120 >/dev/null
mission M_HIGH MONITORING SKENDERIJA SKENDERIJA_K3 120 >/dev/null
wait_mission_status M_LOW QUEUED 5 || fail "M_LOW nije QUEUED"
wait_mission_status M_HIGH QUEUED 5 || fail "M_HIGH nije QUEUED"

HIGH_PRIO="$(central_sql "SELECT mission_priority FROM missions WHERE mission_id='M_HIGH';")"
LOW_PRIO="$(central_sql "SELECT mission_priority FROM missions WHERE mission_id='M_LOW';")"
(( HIGH_PRIO > LOW_PRIO )) || fail "Prioritet MONITORING nije veci od TEST_FLIGHT"

mission STOP M_BLOCK >/dev/null
wait_mission_status M_HIGH ACTIVE 10 || fail "Scheduler nije prvo izabrao visu prioritetnu misiju"
assert_sql_eq "SELECT status FROM missions WHERE mission_id='M_LOW';" "QUEUED" "Niza prioritetna misija nije ostala QUEUED"
pass "Prioritetni scheduler bira visu prioritetnu QUEUED misiju"
