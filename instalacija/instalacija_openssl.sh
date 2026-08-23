#!/usr/bin/env bash
set -Eeuo pipefail

# OpenSSL + PQC setup za SDP projekat autonomnih dronova
# Ubuntu 22.04/24.04
#
# Default verzija se moze promijeniti:
#   OPENSSL_VERSION=3.5.5 ./setup_openssl_pqc.sh
#
# Opcionalno preskoci OpenSSL test suite:
#   SKIP_OPENSSL_TESTS=1 ./setup_openssl_pqc.sh
#
# Ako se skripta pokrene iz root direktorija projekta i postoje
# generate_pqc_certs.sh/build.sh, ponudit ce njihovo pokretanje.

OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.5}"
OPENSSL_PREFIX="${OPENSSL_PREFIX:-/usr/local/openssl}"
OPENSSL_DIR="openssl-${OPENSSL_VERSION}"
OPENSSL_TARBALL="${OPENSSL_DIR}.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/${OPENSSL_TARBALL}"
WORKDIR="${WORKDIR:-/tmp/openssl-pqc-build}"
SKIP_OPENSSL_TESTS="${SKIP_OPENSSL_TESTS:-0}"

log()  { printf '\n\033[1;34m[INFO]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[OK]\033[0m   %s\n' "$*"; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

trap 'die "Greška na liniji $LINENO. Provjeri izlaz iznad."' ERR

if [[ "${EUID}" -eq 0 ]]; then
    SUDO=""
else
    command -v sudo >/dev/null 2>&1 || die "sudo nije instaliran."
    SUDO="sudo"
fi

if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    source /etc/os-release
    log "Sistem: ${PRETTY_NAME:-Linux}"
else
    warn "Ne mogu procitati /etc/os-release; nastavljam."
fi

log "1/8 Instaliram potrebne pakete..."
$SUDO apt-get update
$SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential gcc g++ make perl wget curl git pkg-config ca-certificates \
    libboost-all-dev libsqlite3-dev sqlite3 tcpdump
ok "Osnovni paketi instalirani."

log "2/8 Trenutni sistemski OpenSSL"
if command -v openssl >/dev/null 2>&1; then
    openssl version || true
else
    warn "Sistemski openssl nije pronadjen u PATH-u."
fi

log "3/8 Provjeravam postoji li vec ${OPENSSL_PREFIX}/bin/openssl..."
NEED_BUILD=1
if [[ -x "${OPENSSL_PREFIX}/bin/openssl" ]]; then
    INSTALLED_VERSION="$({ LD_LIBRARY_PATH="${OPENSSL_PREFIX}/lib64:${OPENSSL_PREFIX}/lib:${LD_LIBRARY_PATH:-}" \
        "${OPENSSL_PREFIX}/bin/openssl" version 2>/dev/null || true; } | awk '{print $2}')"
    if [[ "$INSTALLED_VERSION" == "$OPENSSL_VERSION" ]]; then
        ok "OpenSSL ${OPENSSL_VERSION} je vec instaliran u ${OPENSSL_PREFIX}."
        NEED_BUILD=0
    else
        warn "Pronadjen je OpenSSL ${INSTALLED_VERSION:-nepoznata_verzija}; instalirat cu ${OPENSSL_VERSION}."
    fi
fi

if [[ "$NEED_BUILD" -eq 1 ]]; then
    log "4/8 Skidam i kompajliram OpenSSL ${OPENSSL_VERSION}..."
    rm -rf "$WORKDIR"
    mkdir -p "$WORKDIR"
    cd "$WORKDIR"

    wget -O "$OPENSSL_TARBALL" "$OPENSSL_URL"
    tar -xzf "$OPENSSL_TARBALL"
    cd "$OPENSSL_DIR"

    ./Configure \
        --prefix="$OPENSSL_PREFIX" \
        --openssldir="$OPENSSL_PREFIX/ssl" \
        shared

    make -j"$(nproc)"

    if [[ "$SKIP_OPENSSL_TESTS" == "1" ]]; then
        warn "Preskacem 'make test' jer je SKIP_OPENSSL_TESTS=1."
    else
        log "Pokrecem OpenSSL test suite..."
        make test
    fi

    $SUDO make install_sw
    ok "OpenSSL ${OPENSSL_VERSION} instaliran u ${OPENSSL_PREFIX}."
else
    log "4/8 Kompajliranje OpenSSL-a nije potrebno."
fi

log "5/8 Odredjujem direktorij biblioteka..."
if [[ -d "${OPENSSL_PREFIX}/lib64" ]]; then
    OPENSSL_LIBDIR="${OPENSSL_PREFIX}/lib64"
elif [[ -d "${OPENSSL_PREFIX}/lib" ]]; then
    OPENSSL_LIBDIR="${OPENSSL_PREFIX}/lib"
else
    die "Nisam pronasao ${OPENSSL_PREFIX}/lib64 ni ${OPENSSL_PREFIX}/lib."
fi
ok "OpenSSL biblioteke: ${OPENSSL_LIBDIR}"

log "6/8 Podesavam shell okruzenje..."
PROFILE_FILE="/etc/profile.d/openssl-pqc.sh"
$SUDO tee "$PROFILE_FILE" >/dev/null <<PROFILE
# OpenSSL 3.5+ za SDP/PQC projekat
export PATH="${OPENSSL_PREFIX}/bin:\$PATH"
export LD_LIBRARY_PATH="${OPENSSL_LIBDIR}:\${LD_LIBRARY_PATH:-}"
PROFILE
$SUDO chmod 0644 "$PROFILE_FILE"

# Podesi i trenutnu sesiju skripte
export PATH="${OPENSSL_PREFIX}/bin:$PATH"
export LD_LIBRARY_PATH="${OPENSSL_LIBDIR}:${LD_LIBRARY_PATH:-}"

ok "Kreiran ${PROFILE_FILE}. Za nove terminale postavke se ucitavaju automatski."

log "7/8 Provjera OpenSSL-a i PQC algoritama..."
echo
which openssl
openssl version -a

echo
log "Provjera linkovanih libssl/libcrypto biblioteka:"
ldd "${OPENSSL_PREFIX}/bin/openssl" | grep -E 'libssl|libcrypto' || true

echo
log "Provjera ML-DSA potpisa:"
MLDSA_OUT="$(openssl list -signature-algorithms 2>/dev/null | grep -i 'ML-DSA' || true)"
if [[ -n "$MLDSA_OUT" ]]; then
    echo "$MLDSA_OUT"
    ok "ML-DSA algoritmi su dostupni."
else
    die "ML-DSA nije pronadjen. OpenSSL build nema ocekivanu PQC podrsku."
fi

echo
log "Provjera ML-KEM algoritama:"
MLKEM_OUT="$(openssl list -kem-algorithms 2>/dev/null | grep -Ei 'ML-KEM|MLKEM' || true)"
if [[ -n "$MLKEM_OUT" ]]; then
    echo "$MLKEM_OUT"
    ok "ML-KEM algoritmi su dostupni."
else
    warn "ML-KEM nije prikazan kroz 'openssl list -kem-algorithms'. Provjeravam sve algoritme..."
    ALL_MLKEM="$(openssl list -all-algorithms 2>/dev/null | grep -Ei 'ML-KEM|MLKEM' || true)"
    [[ -n "$ALL_MLKEM" ]] || die "ML-KEM nije pronadjen."
    echo "$ALL_MLKEM"
    ok "ML-KEM je dostupan."
fi

# X25519MLKEM768 je TLS named group; dostupnost se na kraju definitivno potvrdi
# pri pokretanju naseg TLS handshake-a, gdje aplikacija ispisuje GROUP=X25519MLKEM768.

log "8/8 Provjera SDP projekta u trenutnom direktoriju..."
PROJECT_DIR="${PROJECT_DIR:-$OLDPWD}"
if [[ -z "${PROJECT_DIR:-}" || ! -d "$PROJECT_DIR" ]]; then
    PROJECT_DIR="$(pwd)"
fi

if [[ -f "$PROJECT_DIR/generate_pqc_certs.sh" && -f "$PROJECT_DIR/build.sh" ]]; then
    ok "Pronadjen SDP projekat u: $PROJECT_DIR"
    cd "$PROJECT_DIR"
    chmod +x generate_pqc_certs.sh build.sh

    if [[ "${AUTO_BUILD_PROJECT:-0}" == "1" ]]; then
        log "AUTO_BUILD_PROJECT=1 -> generisem certifikate i kompajliram projekat..."
        ./generate_pqc_certs.sh
        ./build.sh
        ok "PQC certifikati i projektni binary fajlovi su generisani."
    else
        echo
        echo "Za generisanje certifikata i build projekta pokreni:"
        echo "  cd '$PROJECT_DIR'"
        echo "  ./generate_pqc_certs.sh"
        echo "  ./build.sh"
        echo
        echo "Ili sve automatski ponovi ovako:"
        echo "  AUTO_BUILD_PROJECT=1 $0"
    fi
else
    warn "U trenutnom/originalnom direktoriju nisam pronasao generate_pqc_certs.sh i build.sh."
    echo "To nije greska. Kad prebacis projekat, pokreni iz njegovog root direktorija:"
    echo "  ./generate_pqc_certs.sh"
    echo "  ./build.sh"
fi

echo
ok "Setup zavrsen."
echo "Nova shell sesija automatski koristi:"
echo "  ${OPENSSL_PREFIX}/bin/openssl"
echo
echo "Brza provjera:"
echo "  source ${PROFILE_FILE}"
echo "  which openssl"
echo "  openssl version"
echo "  openssl list -signature-algorithms | grep -i ML-DSA"
echo "  openssl list -kem-algorithms | grep -Ei 'ML-KEM|MLKEM'"
