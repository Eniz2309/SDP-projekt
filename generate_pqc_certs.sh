#!/usr/bin/env bash
set -euo pipefail

if [[ -n "${OPENSSL_BIN:-}" ]]; then
  OPENSSL_BIN="$OPENSSL_BIN"
elif [[ -x /usr/local/openssl/bin/openssl ]]; then
  OPENSSL_BIN=/usr/local/openssl/bin/openssl
else
  OPENSSL_BIN=openssl
fi

if [[ "$OPENSSL_BIN" == /usr/local/openssl/bin/openssl ]]; then
  if [[ -d /usr/local/openssl/lib64 ]]; then
    export LD_LIBRARY_PATH="/usr/local/openssl/lib64:${LD_LIBRARY_PATH:-}"
  elif [[ -d /usr/local/openssl/lib ]]; then
    export LD_LIBRARY_PATH="/usr/local/openssl/lib:${LD_LIBRARY_PATH:-}"
  fi
fi

echo "[PQC] OpenSSL: $($OPENSSL_BIN version)"

if ! $OPENSSL_BIN version | grep -Eq 'OpenSSL 3\.(5|[6-9])|OpenSSL [4-9]\.'; then
  echo "ERROR: Potreban je OpenSSL 3.5.0 ili noviji." >&2
  exit 1
fi

if ! $OPENSSL_BIN list -tls1_3 -tls-groups 2>/dev/null | grep -q 'X25519MLKEM768'; then
  echo "ERROR: OpenSSL nema TLS grupu X25519MLKEM768." >&2
  exit 1
fi

if ! $OPENSSL_BIN list -signature-algorithms 2>/dev/null | grep -qi 'ML-DSA-44'; then
  echo "ERROR: OpenSSL nema ML-DSA-44." >&2
  exit 1
fi

gen_identity() {
  local prefix="$1"
  local cn="$2"

  echo "[PQC] Generisem ${prefix}-key.pem (ML-DSA-44)..."
  $OPENSSL_BIN genpkey \
    -algorithm ML-DSA-44 \
    -out "${prefix}-key.pem"

  echo "[PQC] Generisem ${prefix}-cert.pem..."
  $OPENSSL_BIN req -new -x509 \
    -key "${prefix}-key.pem" \
    -out "${prefix}-cert.pem" \
    -days 365 \
    -subj "/C=BA/ST=Sarajevo/L=Sarajevo/O=SDP.etf/OU=Telekomunikacije/CN=${cn}" \
    -sha256

  chmod 600 "${prefix}-key.pem"
  chmod 644 "${prefix}-cert.pem"
}

gen_identity central central-server
gen_identity regional regional-server

echo
echo "[PQC] Provjera certifikata:"
$OPENSSL_BIN x509 -in central-cert.pem -noout -subject -text | grep -m2 -E 'subject=|Public Key Algorithm'
$OPENSSL_BIN x509 -in regional-cert.pem -noout -subject -text | grep -m2 -E 'subject=|Public Key Algorithm'

echo
echo "Gotovo. Kreirani su:"
echo "  central-key.pem"
echo "  central-cert.pem"
echo "  regional-key.pem"
echo "  regional-cert.pem"
