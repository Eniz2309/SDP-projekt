#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="udp_replay"
source "$(dirname "$0")/common.sh"
setup_test
command -v tcpdump >/dev/null 2>&1 || skip "tcpdump nije instaliran"

# tcpdump za capture obicno zahtijeva root/cap_net_raw. Ako se proces uspjesno
# pokrene, timeout vraca 124; permission error tipicno vraca 1 odmah.
set +e
timeout 1 tcpdump -i lo -w /dev/null "udp" >/dev/null 2>&1
TCPDUMP_CHECK_RC=$?
set -e
if [[ $TCPDUMP_CHECK_RC -ne 0 && $TCPDUMP_CHECK_RC -ne 124 ]]; then
  skip "tcpdump nema potrebne privilegije (pokreni test sa sudo ili dodijeli cap_net_raw)"
fi

start_stack
PCAP="$RESULT_DIR/one_udp_packet.pcap"
tcpdump -i lo -U -c 1 -w "$PCAP" "udp dst port $REGIONAL_UDP_PORT" >"$RESULT_DIR/tcpdump.log" 2>&1 &
TCPDUMP_PID=$!
CHILD_PIDS+=("$TCPDUMP_PID")

start_drone DRON_REPLAY abc123 120
wait_drone_available DRON_REPLAY 12 || fail "DRON_REPLAY nije AVAILABLE"

# Sacekaj da tcpdump zavrsi nakon prvog TELEMETRY/KEEPALIVE paketa.
local_end=$((SECONDS + 10))
while kill -0 "$TCPDUMP_PID" 2>/dev/null && (( SECONDS < local_end )); do sleep 0.2; done
kill "$TCPDUMP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
[[ -s "$PCAP" ]] || fail "Nije snimljen UDP paket"

python3 "$TESTS_DIR/helpers/replay_pcap_udp.py" "$PCAP" "$REGIONAL_HOST" "$REGIONAL_UDP_PORT" >"$RESULT_DIR/replay.log"
wait_log "$REGIONAL_LOG" "[REGIONAL][UDP][ANTI-REPLAY] Odbijen DRON_REPLAY" 6 || fail "Replay nije odbijen"
pass "AES-GCM UDP anti-replay SEQ"
