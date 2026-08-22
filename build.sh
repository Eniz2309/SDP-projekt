#!/usr/bin/env bash
set -euo pipefail

CXX="${CXX:-g++}"
if [[ -x /usr/local/openssl/bin/openssl ]]; then
  OPENSSL_CHECK=/usr/local/openssl/bin/openssl
else
  OPENSSL_CHECK=openssl
fi
COMMON=(-std=c++11 -I . -pthread)
SSL_FLAGS=(-lssl -lcrypto)

# Laboratorijska instalacija OpenSSL 3.5 je najcesce u /usr/local/openssl.
if [[ -f /usr/local/openssl/include/openssl/ssl.h ]]; then
  COMMON+=(-I/usr/local/openssl/include)

  if [[ -d /usr/local/openssl/lib64 ]]; then
    SSL_FLAGS=(-L/usr/local/openssl/lib64 -Wl,-rpath,/usr/local/openssl/lib64 -lssl -lcrypto)
  elif [[ -d /usr/local/openssl/lib ]]; then
    SSL_FLAGS=(-L/usr/local/openssl/lib -Wl,-rpath,/usr/local/openssl/lib -lssl -lcrypto)
  fi
fi

echo "[BUILD] $($CXX --version | head -1)"
echo "[BUILD] $($OPENSSL_CHECK version)"

$CXX "${COMMON[@]}" central_server.cpp \
  -o central_server -lboost_system -lsqlite3 "${SSL_FLAGS[@]}"

$CXX "${COMMON[@]}" regional_server.cpp \
  -o regional_server -lboost_system -lsqlite3 "${SSL_FLAGS[@]}"

$CXX "${COMMON[@]}" drone_client.cpp \
  -o drone_client -lboost_system "${SSL_FLAGS[@]}"

$CXX "${COMMON[@]}" mission_client.cpp \
  -o mission_client -lboost_system "${SSL_FLAGS[@]}"

echo "[BUILD] Gotovo: central_server regional_server drone_client mission_client"
