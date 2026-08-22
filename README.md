# Protokol za upravljanje autonomnim dronovima

Studentski projekat protokola za hijerarhijsko upravljanje autonomnim dronovima preko centralnog i regionalnog servera.

## Arhitektura

```text
CENTRAL SERVER
      |
      | TLS 1.3 + PQC
      v
REGIONAL SERVER
      |
      | TLS 1.3 + PQC / AES-256-GCM UDP
      v
    DRONOVI
```

Dronovi komuniciraju iskljucivo sa pripadajucim regionalnim serverom. Direktna komunikacija dron-dron i dron-centralni server nije dozvoljena.

## Glavne funkcionalnosti

- registracija drona pomocu `DRONE_URI` i sigurnosnog tokena
- centralna provjera URI+TOKEN credentiala
- registar dronova i njihovih statusa
- MONITORING, DELIVERY, INSPECTION i TEST_FLIGHT misije
- prioritetni red zadataka
- FORMATION sa regionalnim serverom kao virtualnim leaderom
- STOP_MISSION, promjena parametara leta i rucni RTB
- LOW_BATTERY i CONNECTION_LOST alarmi
- UDP TELEMETRY i KEEPALIVE
- SQLite evidencija dronova, misija, alarma, zona i inspection izvjestaja
- TLS 1.3 sa `X25519MLKEM768` i `ML-DSA-44`
- AES-256-GCM zastita UDP payload-a kljucem izvedenim iz TLS sesije

## Fajlovi

```text
central_server.cpp
regional_server.cpp
drone_client.cpp
mission_client.cpp
pqc_tls_utils.h
udp_aead.h
build.sh
generate_pqc_certs.sh
drone_credentials.example.conf
```

Projekt takodjer ocekuje postojece:

```text
json/json.h
sqlite3_wrapper.h
```

## Credential registar

Centralni server ucitava `drone_credentials.conf`.

Prije prvog pokretanja napravi lokalni credential fajl:

```bash
cp drone_credentials.example.conf drone_credentials.conf
```

Format:

```text
DRONE_URI TOKEN
```

Centralni server ne cuva raw token u SQLite bazi. Cuva SHA-256 hash i provjerava URI, token, `enabled` stanje i region kojem je dron vezan.

`drone_credentials.conf` je u `.gitignore` i ne treba ga commitovati ako sadrzi stvarne tokene.

## Build

Potreban je OpenSSL 3.5.0 ili noviji sa podrskom za `X25519MLKEM768` i `ML-DSA-44`, Boost.Asio/Boost.System i SQLite3.

```bash
chmod +x generate_pqc_certs.sh build.sh
./generate_pqc_certs.sh
./build.sh
```

## Pokretanje

Centralni server:

```bash
./central_server 9000
```

Regionalni server:

```bash
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 \
  43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4
```

Dron:

```bash
./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120
```

Operator / mission client:

```bash
./mission_client 127.0.0.1 8000 M001 MONITORING SKENDERIJA SKENDERIJA_K2 120
./mission_client 127.0.0.1 8000 M003 DELIVERY SKENDERIJA AUTO 120 43.8580 18.4160
./mission_client 127.0.0.1 8000 M_FORM FORMATION SKENDERIJA SKENDERIJA_K2 120 3 10
./mission_client 127.0.0.1 8000 STOP M001
./mission_client 127.0.0.1 8000 PARAMS DRON_001 150 15 EAST
./mission_client 127.0.0.1 8000 RTB DRON_001
```

## Autentifikacija

Tok registracije i autentifikacije:

```text
DRON -> REGISTER_REQ -> REGIONAL -> DRONE_REGISTER -> CENTRAL
CENTRAL -> provjera URI + token hash + region
CENTRAL -> DRONE_REGISTER_OK -> REGIONAL -> REGISTER_ACK -> DRON

DRON -> AUTH_REQ -> REGIONAL -> DRONE_AUTH -> CENTRAL
CENTRAL -> ponovna provjera credentiala
CENTRAL -> DRONE_AUTH_OK -> REGIONAL -> AUTH_ACK -> DRON
```

Tek nakon uspjesnog `AUTH_ACK` dron prelazi u `AVAILABLE` i pokrece TELEMETRY/KEEPALIVE.

### Negativni testovi

Pogresan token:

```bash
./drone_client 127.0.0.1 8000 8001 DRON_001 POGRESAN_TOKEN 120
```

Ocekivano: `INVALID_TOKEN`.

Nepoznat URI:

```bash
./drone_client 127.0.0.1 8000 8001 DRON_HACKER abc123 120
```

Ocekivano: `UNKNOWN_DRONE_URI`.

## Provjera baze

```bash
sqlite3 central_server.db
```

Primjer:

```sql
SELECT drone_uri, enabled, registered_region, created_at, last_auth_at
FROM drone_credentials;
```

## Mrezna sigurnost

TCP kanali koriste TLS 1.3. Ocekivani parametri sesije su:

```text
TLS=TLSv1.3
GROUP=X25519MLKEM768
CIPHER=TLS_AES_256_GCM_SHA384
PEER_KEY=ML-DSA-44
```

UDP TELEMETRY/KEEPALIVE se salju kao AES-256-GCM envelope sa `NONCE`, `TAG` i `CIPHERTEXT` poljima.
