#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument('--cwd', required=True)
ap.add_argument('--exe', required=True)
ap.add_argument('--cert', required=True)
ap.add_argument('--host', required=True)
ap.add_argument('--tcp-port', required=True)
ap.add_argument('--udp-port', required=True)
ap.add_argument('--uri', required=True)
ap.add_argument('--token', required=True)
args = ap.parse_args()

env = os.environ.copy()
env['SDP_REGIONAL_CERT'] = args.cert
start = time.perf_counter()
p = subprocess.Popen(
    ['stdbuf', '-oL', '-eL', args.exe, args.host, args.tcp_port, args.udp_port, args.uri, args.token, '120'],
    cwd=args.cwd,
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1,
)
try:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        line = p.stdout.readline()
        if not line:
            if p.poll() is not None:
                break
            continue
        if 'Central confirms AVAILABLE state.' in line:
            ms = (time.perf_counter() - start) * 1000.0
            print(f'{ms:.3f}')
            sys.exit(0)
    raise RuntimeError('AUTH/AVAILABLE potvrda nije stigla')
finally:
    p.terminate()
    try:
        p.wait(timeout=2)
    except subprocess.TimeoutExpired:
        p.kill()
