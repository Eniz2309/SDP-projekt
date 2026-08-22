#!/usr/bin/env bash
set -euo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TESTS_DIR/.." && pwd)"

CENTRAL_HOST="${CENTRAL_HOST:-127.0.0.1}"
CENTRAL_PORT="${CENTRAL_PORT:-9000}"
REGIONAL_HOST="${REGIONAL_HOST:-127.0.0.1}"
REGIONAL_TCP_PORT="${REGIONAL_TCP_PORT:-8000}"
REGIONAL_UDP_PORT="${REGIONAL_UDP_PORT:-8001}"
REGION_ID="${REGION_ID:-REGION_SARAJEVO}"
BASE_LAT="${BASE_LAT:-43.8563}"
BASE_LON="${BASE_LON:-18.4131}"
ZONES_CONFIG="${ZONES_CONFIG:-SKENDERIJA:43.8563:18.4131:1000:4}"
TEST_CREDENTIALS="${TEST_CREDENTIALS:-$TESTS_DIR/test_credentials.conf}"
WAIT_TIMEOUT="${WAIT_TIMEOUT:-20}"

TEST_NAME="${TEST_NAME:-$(basename "${BASH_SOURCE[1]:-manual}" .sh)}"
RESULT_ROOT="$TESTS_DIR/results"
RESULT_DIR="$RESULT_ROOT/$TEST_NAME"
WORK_DIR="$RESULT_DIR/work"
CENTRAL_LOG="$RESULT_DIR/central.log"
REGIONAL_LOG="$RESULT_DIR/regional.log"

CENTRAL_PID=""
REGIONAL_PID=""
declare -a CHILD_PIDS=()
declare -A DRONE_PIDS=()
declare -A DRONE_LOGS=()

log() { printf '[%s] %s\n' "$TEST_NAME" "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; return 1; }
skip() { printf '[SKIP] %s\n' "$*"; exit 77; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "Nedostaje komanda: $1"
}

preflight() {
  require_cmd sqlite3
  require_cmd python3
  require_cmd ss
  require_cmd grep
  require_cmd awk
  require_cmd sed
  require_cmd timeout

  for f in central_server regional_server drone_client mission_client; do
    [[ -x "$PROJECT_ROOT/$f" ]] || fail "Nedostaje executable $f. Prvo pokreni ./build.sh"
  done

  for f in central-cert.pem central-key.pem regional-cert.pem regional-key.pem; do
    [[ -f "$PROJECT_ROOT/$f" ]] || fail "Nedostaje $f. Prvo pokreni ./generate_pqc_certs.sh"
  done

  [[ -f "$TEST_CREDENTIALS" ]] || fail "Nedostaje $TEST_CREDENTIALS"
}

port_listening_tcp() {
  local port="$1"
  ss -ltnH 2>/dev/null | awk '{print $4}' | grep -Eq "[:.]${port}$"
}

port_listening_udp() {
  local port="$1"
  ss -lunH 2>/dev/null | awk '{print $4}' | grep -Eq "[:.]${port}$"
}

assert_ports_free() {
  if port_listening_tcp "$CENTRAL_PORT"; then fail "TCP port $CENTRAL_PORT je zauzet"; fi
  if port_listening_tcp "$REGIONAL_TCP_PORT"; then fail "TCP port $REGIONAL_TCP_PORT je zauzet"; fi
  if port_listening_udp "$REGIONAL_UDP_PORT"; then fail "UDP port $REGIONAL_UDP_PORT je zauzet"; fi
}

wait_tcp_listen() {
  local port="$1" timeout_s="${2:-$WAIT_TIMEOUT}"
  local end=$((SECONDS + timeout_s))
  while (( SECONDS < end )); do
    port_listening_tcp "$port" && return 0
    sleep 0.2
  done
  return 1
}

wait_udp_listen() {
  local port="$1" timeout_s="${2:-$WAIT_TIMEOUT}"
  local end=$((SECONDS + timeout_s))
  while (( SECONDS < end )); do
    port_listening_udp "$port" && return 0
    sleep 0.2
  done
  return 1
}

prepare_test_dir() {
  rm -rf "$RESULT_DIR"
  mkdir -p "$WORK_DIR"
}

cleanup() {
  set +e
  local pid
  for pid in "${CHILD_PIDS[@]:-}"; do
    [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
  done
  [[ -n "$REGIONAL_PID" ]] && kill "$REGIONAL_PID" 2>/dev/null || true
  [[ -n "$CENTRAL_PID" ]] && kill "$CENTRAL_PID" 2>/dev/null || true
  sleep 0.4
  for pid in "${CHILD_PIDS[@]:-}" "$REGIONAL_PID" "$CENTRAL_PID"; do
    [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  set -e
}

setup_test() {
  preflight
  assert_ports_free
  prepare_test_dir
  trap cleanup EXIT INT TERM
}

start_stack() {
  log "Pokrecem centralni i regionalni server..."

  (
    cd "$WORK_DIR"
    exec env \
      SDP_DRONE_CREDENTIALS_FILE="$TEST_CREDENTIALS" \
      SDP_CENTRAL_CERT="$PROJECT_ROOT/central-cert.pem" \
      SDP_CENTRAL_KEY="$PROJECT_ROOT/central-key.pem" \
      stdbuf -oL -eL "$PROJECT_ROOT/central_server" "$CENTRAL_PORT"
  ) >"$CENTRAL_LOG" 2>&1 &
  CENTRAL_PID=$!

  wait_tcp_listen "$CENTRAL_PORT" 10 || {
    tail -100 "$CENTRAL_LOG" >&2 || true
    fail "Centralni server se nije pokrenuo"
  }

  (
    cd "$WORK_DIR"
    exec env \
      SDP_CENTRAL_CERT="$PROJECT_ROOT/central-cert.pem" \
      SDP_REGIONAL_CERT="$PROJECT_ROOT/regional-cert.pem" \
      SDP_REGIONAL_KEY="$PROJECT_ROOT/regional-key.pem" \
      stdbuf -oL -eL "$PROJECT_ROOT/regional_server" \
      "$REGION_ID" "$CENTRAL_HOST" "$CENTRAL_PORT" \
      "$REGIONAL_TCP_PORT" "$REGIONAL_UDP_PORT" \
      "$BASE_LAT" "$BASE_LON" "$ZONES_CONFIG"
  ) >"$REGIONAL_LOG" 2>&1 &
  REGIONAL_PID=$!

  wait_tcp_listen "$REGIONAL_TCP_PORT" 10 || {
    tail -100 "$REGIONAL_LOG" >&2 || true
    fail "Regionalni TCP server se nije pokrenuo"
  }
  wait_udp_listen "$REGIONAL_UDP_PORT" 10 || {
    tail -100 "$REGIONAL_LOG" >&2 || true
    fail "Regionalni UDP server se nije pokrenuo"
  }
}

start_drone() {
  local uri="$1" token="$2" altitude="${3:-120}"
  shift 3 || true
  local log_file="$RESULT_DIR/${uri}.log"

  (
    cd "$WORK_DIR"
    exec env \
      SDP_REGIONAL_CERT="$PROJECT_ROOT/regional-cert.pem" \
      "$@" \
      stdbuf -oL -eL "$PROJECT_ROOT/drone_client" \
      "$REGIONAL_HOST" "$REGIONAL_TCP_PORT" "$REGIONAL_UDP_PORT" \
      "$uri" "$token" "$altitude"
  ) >"$log_file" 2>&1 &

  local pid=$!
  CHILD_PIDS+=("$pid")
  DRONE_PIDS["$uri"]="$pid"
  DRONE_LOGS["$uri"]="$log_file"
  log "Pokrenut $uri (pid=$pid)"
}

run_drone_timed() {
  local uri="$1" token="$2" outfile="$3" timeout_s="${4:-6}"
  set +e
  (
    cd "$WORK_DIR"
    env SDP_REGIONAL_CERT="$PROJECT_ROOT/regional-cert.pem" \
      timeout "$timeout_s" "$PROJECT_ROOT/drone_client" \
      "$REGIONAL_HOST" "$REGIONAL_TCP_PORT" "$REGIONAL_UDP_PORT" \
      "$uri" "$token" 120
  ) >"$outfile" 2>&1
  local rc=$?
  set -e
  return "$rc"
}

mission() {
  (
    cd "$WORK_DIR"
    env SDP_REGIONAL_CERT="$PROJECT_ROOT/regional-cert.pem" \
      "$PROJECT_ROOT/mission_client" "$REGIONAL_HOST" "$REGIONAL_TCP_PORT" "$@"
  )
}

central_sql() {
  sqlite3 -noheader -batch "$WORK_DIR/central_server.db" "$1"
}

regional_sql() {
  sqlite3 -noheader -batch "$WORK_DIR/${REGION_ID}_regional_server.db" "$1"
}

wait_sql() {
  local sql="$1" expected="$2" timeout_s="${3:-$WAIT_TIMEOUT}"
  local end=$((SECONDS + timeout_s)) value=""
  while (( SECONDS < end )); do
    value="$(central_sql "$sql" 2>/dev/null || true)"
    [[ "$value" == "$expected" ]] && return 0
    sleep 0.25
  done
  log "SQL timeout. Ocekivano='$expected', zadnje='$value'"
  return 1
}

wait_sql_nonzero() {
  local sql="$1" timeout_s="${2:-$WAIT_TIMEOUT}"
  local end=$((SECONDS + timeout_s)) value="0"
  while (( SECONDS < end )); do
    value="$(central_sql "$sql" 2>/dev/null || echo 0)"
    [[ "$value" =~ ^[0-9]+$ ]] && (( value > 0 )) && return 0
    sleep 0.25
  done
  log "SQL timeout. Zadnja vrijednost='$value'"
  return 1
}

wait_log() {
  local file="$1" pattern="$2" timeout_s="${3:-$WAIT_TIMEOUT}"
  local end=$((SECONDS + timeout_s))
  while (( SECONDS < end )); do
    [[ -f "$file" ]] && grep -Fq "$pattern" "$file" && return 0
    sleep 0.25
  done
  return 1
}

wait_drone_available() {
  local uri="$1" timeout_s="${2:-$WAIT_TIMEOUT}"
  wait_sql "SELECT status FROM drones WHERE drone_uri='$uri';" "AVAILABLE" "$timeout_s"
}

wait_mission_status() {
  local mid="$1" status="$2" timeout_s="${3:-$WAIT_TIMEOUT}"
  wait_sql "SELECT status FROM missions WHERE mission_id='$mid';" "$status" "$timeout_s"
}

stop_mission_if_active() {
  local mid="$1"
  local s="$(central_sql "SELECT status FROM missions WHERE mission_id='$mid';" 2>/dev/null || true)"
  if [[ "$s" == "ACTIVE" ]]; then
    mission STOP "$mid" >/dev/null
    wait_sql "SELECT status FROM missions WHERE mission_id='$mid';" "STOPPED" 10 || true
  fi
}

assert_eq() {
  local got="$1" expected="$2" msg="$3"
  [[ "$got" == "$expected" ]] || fail "$msg (got='$got', expected='$expected')"
}

assert_contains_file() {
  local file="$1" pattern="$2" msg="$3"
  grep -Fq "$pattern" "$file" || {
    tail -80 "$file" >&2 || true
    fail "$msg"
  }
}

assert_sql_eq() {
  local sql="$1" expected="$2" msg="$3"
  local got="$(central_sql "$sql")"
  assert_eq "$got" "$expected" "$msg"
}
