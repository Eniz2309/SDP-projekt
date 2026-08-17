# v12_pqc_tls - quantum-safe komunikacija (TLS + PQC)

Ova verzija nastavlja v11_control_commands i zadrzava sve prethodne funkcionalnosti:
- registracija/autentifikacija dronova
- UDP keepalive/telemetrija
- scheduler i prioriteti
- MONITORING, DELIVERY, INSPECTION, TEST_FLIGHT
- STOP_MISSION
- watchdog / LOW_BATTERY / CONNECTION_LOST
- FORMATION (regionalni server = virtual leader)
- PARAMS i rucni RTB

NOVO U v12
=========

1) TCP je quantum-safe TLS 1.3
--------------------------------
Svi TCP kanali su prebaceni sa plain TCP-a na Boost.Asio SSL stream:

  regionalni <-> centralni
  dron        <-> regionalni
  mission_cli <-> regionalni

TLS konfiguracija:
  TLS verzija: TLS 1.3 ONLY
  KEM/grupa:   X25519MLKEM768
  potpis:      ML-DSA-44

X25519MLKEM768 je hibrid klasicnog X25519 i post-kvantnog ML-KEM-768.
Serveri koriste ML-DSA-44 certifikate.

2) UDP ostaje UDP, ali payload vise nije otvoreni JSON
------------------------------------------------------
TELEMETRY i KEEPALIVE i dalje koriste UDP zbog zahtjeva projekta za data-stream/datagram
komunikacijom i periodickim podacima.

Nakon PQC TLS handshake-a dron i regionalni server pozivaju TLS exporter i izvode isti
256-bitni sesijski kljuc. Taj kljuc se koristi za AES-256-GCM zastitu UDP payload-a.

Na mrezi se vidi samo envelope oblika:
  TYPE=PQC_UDP
  DRONE_URI=DRON_001
  NONCE=...
  TAG=...
  CIPHERTEXT=...

Originalni TELEMETRY/KEEPALIVE JSON se nalazi unutar CIPHERTEXT i autentifikovan je
AES-GCM tagom.

ZAHTJEVI
========

- OpenSSL 3.5.0 ili noviji
- Boost.Asio / Boost.System
- SQLite3
- postojeci json/json.h i sqlite3_wrapper.h iz projekta

Provjera:

  openssl version -a
  openssl list -tls1_3 -tls-groups | grep X25519MLKEM768
  openssl list -signature-algorithms | grep -i ML-DSA-44

Ako X25519MLKEM768 ili ML-DSA-44 ne postoje, prvo instalirati OpenSSL 3.5+ prema
laboratorijskoj vjezbi TLS-PQC.

FAJLOVI
=======

  dostava_central_server_v12_pqc_tls.cpp
  dostava_regional_server_v12_pqc_tls.cpp
  dostava_drone_client_v12_pqc_tls.cpp
  mission_client_v12_pqc_tls.cpp
  pqc_tls_utils.h
  udp_aead.h
  generate_pqc_certs_v12.sh
  build_v12_pqc_tls.sh

CERTIFIKATI
===========

U root direktoriju projekta:

  chmod +x generate_pqc_certs_v12.sh
  ./generate_pqc_certs_v12.sh

Dobijes:

  central-key.pem
  central-cert.pem
  regional-key.pem
  regional-cert.pem

Privatne *.key.pem fajlove NE stavljati u javni Git repozitorij.
Za projekat je bolje commitovati skriptu koja ih generise.

BUILD
=====

  chmod +x build_v12_pqc_tls.sh
  ./build_v12_pqc_tls.sh

Rucno, ako je OpenSSL 3.5 u /usr/local/openssl:

  g++ -std=c++11 -I . -I/usr/local/openssl/include \
      dostava_central_server_v12_pqc_tls.cpp -o central_server \
      -lboost_system -lsqlite3 -L/usr/local/openssl/lib64 \
      -Wl,-rpath,/usr/local/openssl/lib64 -lssl -lcrypto -pthread

Analogno se kompajliraju regional_server, drone_client i mission_client.

POKRETANJE
==========

1. Centralni:

  ./central_server 9000

Centralni po defaultu ucitava:
  central-cert.pem
  central-key.pem

2. Regionalni:

  ./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 \
      43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4

Regionalni:
- kao TLS klijent vjeruje central-cert.pem
- kao TLS server koristi regional-cert.pem + regional-key.pem

3. Dron:

  ./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120

Dron vjeruje regional-cert.pem.

Pokreni vise dronova u zasebnim terminalima:

  ./drone_client 127.0.0.1 8000 8001 DRON_002 abc123 120
  ./drone_client 127.0.0.1 8000 8001 DRON_003 abc123 120

4. Operator / mission client:

  ./mission_client 127.0.0.1 8000 M001 MONITORING SKENDERIJA SKENDERIJA_K2 120
  ./mission_client 127.0.0.1 8000 PARAMS DRON_001 150 15 EAST
  ./mission_client 127.0.0.1 8000 RTB DRON_001

Sve ove komande sada prema regionalnom idu kroz PQC TLS.

OCEKIVANI PQC ISPIS
===================

Dron / regionalni / mission client treba da ispisu nesto poput:

  TLS=TLSv1.3
  GROUP=X25519MLKEM768
  CIPHER=TLS_AES_256_GCM_SHA384
  PEER_KEY=ML-DSA-44

To je najjednostavniji dokaz na demonstraciji da je dogovorena PQC/hibridna TLS sesija.

Dodatna provjera centralnog porta:

  openssl s_client -connect 127.0.0.1:9000 \
      -tls1_3 \
      -groups X25519MLKEM768 \
      -sigalgs ML-DSA-44 \
      -CAfile central-cert.pem

Traziti u ispisu:
  Protocol: TLSv1.3
  Negotiated TLS1.3 group: X25519MLKEM768
  Peer signature type: mldsa44 / ML-DSA-44

WIRESHARK / PCAP
================

Postavka projekta trazi .pcap finalnog protokola. Primjer na jednoj masini:

  sudo tcpdump -i lo -w v12_pqc_demo.pcap \
      'tcp port 9000 or tcp port 8000 or udp port 8001'

Na dvije udaljene masine zamijeniti "lo" odgovarajucim interfejsom (npr. ens160/eth0).

Kod TCP prometa JSON vise ne treba biti citljiv jer je unutar TLS Application Data zapisa.
Kod UDP prometa TELEMETRY JSON vise nije otvoren tekst; vidi se PQC_UDP envelope sa
AES-GCM ciphertextom.

ASINHRONA I SINHRONA KOMUNIKACIJA
=================================

- CENTRALNI: async_accept, async_handshake, async_read_until i async_write.
- DRON: async_connect, async_handshake, async_read_until i async_write.
- REGIONALNI -> CENTRALNI: sinhroni TLS write/read jer regionalni za pojedine kontrolne
  odluke odmah ceka odgovor centralnog.
- UDP telemetry/keepalive: datagramska komunikacija, bez TCP byte-stream framinga.

NAPOMENA O CERTIFIKATIMA
========================

Ovo je studentska/demo PKI konfiguracija sa samopotpisanim ML-DSA-44 certifikatima.
Klijenti eksplicitno ucitavaju odgovarajuci serverski certifikat kao trust anchor.
Autentifikacija drona URI+TOKEN ostaje na aplikacijskom nivou, dok TLS/PQC stiti kanal.
