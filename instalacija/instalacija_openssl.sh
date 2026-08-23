#!/usr/bin/env bash
set -e

OPENSSL_VERSION="3.5.7"
OPENSSL_PREFIX="/usr/local/openssl"

echo "============================================================"
echo " SDP - Ubuntu 22.04 OpenSSL/PQC setup"
echo " OpenSSL: ${OPENSSL_VERSION}"
echo "============================================================"

echo
echo "[1/7] Azuriranje APT paketa..."
sudo apt update

echo
echo "[2/7] Instalacija potrebnih paketa..."
sudo apt install -y \
    build-essential \
    gcc \
    g++ \
    make \
    perl \
    wget \
    curl \
    git \
    pkg-config \
    libboost-all-dev \
    libsqlite3-dev \
    sqlite3 \
    tcpdump \
    ca-certificates

echo
echo "[3/7] Preuzimanje OpenSSL ${OPENSSL_VERSION}..."
cd /tmp
rm -rf "openssl-${OPENSSL_VERSION}" "openssl-${OPENSSL_VERSION}.tar.gz"

wget "https://www.openssl-library.org/source/openssl-${OPENSSL_VERSION}.tar.gz"

echo
echo "[4/7] Raspakivanje i kompajliranje..."
tar -xzf "openssl-${OPENSSL_VERSION}.tar.gz"
cd "openssl-${OPENSSL_VERSION}"

./Configure \
    --prefix="${OPENSSL_PREFIX}" \
    --openssldir="${OPENSSL_PREFIX}/ssl" \
    shared

make -j"$(nproc)"

echo
echo "[5/7] Instalacija OpenSSL-a..."
sudo make install_sw

echo
echo "[6/7] Podesavanje PATH i biblioteka..."

sudo tee /etc/profile.d/openssl-pqc.sh >/dev/null <<'EOF'
export PATH=/usr/local/openssl/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/openssl/lib64:/usr/local/openssl/lib:${LD_LIBRARY_PATH:-}
EOF

sudo chmod 644 /etc/profile.d/openssl-pqc.sh

export PATH=/usr/local/openssl/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/openssl/lib64:/usr/local/openssl/lib:${LD_LIBRARY_PATH:-}

echo
echo "[7/7] Provjera instalacije..."
echo

echo "OpenSSL binary:"
which openssl

echo
echo "OpenSSL verzija:"
openssl version -a

echo
echo "ML-DSA algoritmi:"
openssl list -signature-algorithms | grep -i "ML-DSA" || true

echo
echo "ML-KEM algoritmi:"
openssl list -kem-algorithms | grep -i "ML-KEM" || true

echo
echo "TLS/PQC grupe:"
openssl list -tls1_3 -tls-groups 2>/dev/null | grep -Ei "MLKEM|X25519MLKEM" || true

echo
echo "Linkovane OpenSSL biblioteke:"
ldd "${OPENSSL_PREFIX}/bin/openssl" | grep -E "ssl|crypto" || true

echo
echo "============================================================"
echo " GOTOVO"
echo "============================================================"
echo
echo "Za trenutni terminal pokreni:"
echo
echo "  source /etc/profile.d/openssl-pqc.sh"
echo
echo "Zatim provjeri:"
echo
echo "  which openssl"
echo "  openssl version"
echo
echo "Ako si u root folderu SDP projekta:"
echo
echo "  ./generate_pqc_certs.sh"
echo "  ./build.sh"
echo
