#!/usr/bin/env bash
set -euo pipefail
TEST_NAME="benchmark"
source "$(dirname "$0")/common.sh"
setup_test
start_stack

ITERATIONS="${ITERATIONS:-10}"
CSV="$RESULT_DIR/benchmark_results.csv"
echo "metric,iteration,milliseconds" >"$CSV"

now_ns() { date +%s%N; }
measure_shell_ms() {
  local metric="$1" iteration="$2"; shift 2
  local start end ms
  start="$(now_ns)"
  "$@" >/dev/null
  end="$(now_ns)"
  ms="$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.3f", (b-a)/1000000.0}')"
  echo "$metric,$iteration,$ms" >>"$CSV"
}

log "Benchmark AUTH/AVAILABLE latency ($ITERATIONS iteracija)"
for i in $(seq 1 "$ITERATIONS"); do
  ms="$(python3 "$TESTS_DIR/helpers/measure_auth.py" \
    --cwd "$WORK_DIR" --exe "$PROJECT_ROOT/drone_client" \
    --cert "$PROJECT_ROOT/regional-cert.pem" \
    --host "$REGIONAL_HOST" --tcp-port "$REGIONAL_TCP_PORT" --udp-port "$REGIONAL_UDP_PORT" \
    --uri DRON_BENCH --token abc123)"
  echo "auth_available,$i,$ms" >>"$CSV"
  sleep 0.2
done

# Stabilan dron za kontrolne transakcije.
start_drone DRON_001 abc123 120
wait_drone_available DRON_001 12 || fail "Benchmark dron nije AVAILABLE"
mission M_BENCH_ACTIVE MONITORING SKENDERIJA SKENDERIJA_K1 120 >/dev/null
wait_mission_status M_BENCH_ACTIVE ACTIVE 8 || fail "Benchmark misija nije ACTIVE"

log "Benchmark PARAMS request/response latency"
for i in $(seq 1 "$ITERATIONS"); do
  alt=$((140 + (i % 5) * 2))
  measure_shell_ms params "$i" mission PARAMS DRON_001 "$alt" 15 EAST
done

log "Benchmark MISSION_SUBMIT request/response latency"
for i in $(seq 1 "$ITERATIONS"); do
  measure_shell_ms mission_submit "$i" mission "M_BENCH_Q_$i" TEST_FLIGHT SKENDERIJA SKENDERIJA_K3 120
done

measure_shell_ms stop 1 mission STOP M_BENCH_ACTIVE
wait_mission_status M_BENCH_ACTIVE STOPPED 10 || true
wait_drone_available DRON_001 10 || true
measure_shell_ms rtb 1 mission RTB DRON_001

python3 - "$CSV" <<'PY'
import csv, statistics, sys
from collections import defaultdict
path = sys.argv[1]
vals = defaultdict(list)
with open(path, newline='') as f:
    for row in csv.DictReader(f):
        vals[row['metric']].append(float(row['milliseconds']))
print('\nBenchmark summary')
print('metric,count,avg_ms,p50_ms,p95_ms,min_ms,max_ms')
for metric, xs in vals.items():
    ys = sorted(xs)
    p95 = ys[max(0, min(len(ys)-1, int(round(0.95*(len(ys)-1)))))]
    print(f"{metric},{len(xs)},{statistics.mean(xs):.3f},{statistics.median(xs):.3f},{p95:.3f},{min(xs):.3f},{max(xs):.3f}")
PY

pass "Benchmark CSV: $CSV"
