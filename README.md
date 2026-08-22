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
- UDP TELEMETRY i KEEPALIVE sa zadatkom, rezimom leta i statusom senzora
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

## Telemetrija

Dron periodicki salje `TELEMETRY` i `KEEPALIVE` regionalnom serveru preko AES-256-GCM zasticenog UDP kanala. Trenutni operativni podaci ukljucuju:

```text
DRONE_URI
BATTERY
STATUS
LAT / LON
ALTITUDE
SPEED
DIRECTION
ROUTE_ID
MISSION_ID
MISSION_TYPE
FLIGHT_MODE
SENSOR_STATUS
```

`MISSION_ID` i `MISSION_TYPE` opisuju trenutni zadatak. `FLIGHT_MODE` odvaja rezim leta (`GROUND`, `STANDBY`, `AUTONOMOUS`, `FORMATION`, `RTB`) od tipa zadatka, dok je `SENSOR_STATUS` simulirani health status senzorskog podsistema. Regionalni server cuva trenutno stanje i historijski log, a zatim isti status prosljedjuje centralnom serveru.

Postojece SQLite baze se automatski dopunjavaju novim kolonama pri pokretanju servera.

## Provjera baze

```bash
sqlite3 central_server.db
```

Primjer:

```sql
SELECT drone_uri, enabled, registered_region, created_at, last_auth_at
FROM drone_credentials;

SELECT drone_uri, status, mission_id, mission_type, flight_mode, sensor_status,
       speed, direction, route_id, battery, lat, lon, altitude
FROM drones;
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

## Detekcija konflikta ruta

Centralni scheduler prije aktivacije misije gradi geometriju stvarne planirane putanje i poredi je sa svim aktivnim misijama u istom regionu. Konture se modeliraju kao segmenti kvadratne rute, dok se DELIVERY modelira kao stvarna putanja `trenutna pozicija -> izlaz sa konture -> dostavna tacka`.

Konflikt postoji kada su dvije putanje blize od horizontalne sigurnosne udaljenosti, a istovremeno nemaju dovoljno vertikalno razdvajanje. Scheduler tada automatski proba sljedeci visinski slot. Ako nijedan slot nije siguran, misija ostaje `QUEUED` i u bazi/odgovoru dobija `QUEUE_REASON=ROUTE_CONFLICT_NO_SAFE_ALTITUDE`. Nema prekidanja vec aktivne misije.

Ista provjera se koristi i za rucni `PARAMS` zahtjev za promjenu visine. Ako bi nova visina napravila konflikt sa drugom aktivnom putanjom, centralni vraca `CONTROL_REJECTED` sa razlogom `ROUTE_CONFLICT_AT_REQUESTED_ALTITUDE`. Visinu clana aktivne formacije operator ne mijenja pojedinacno jer je zajednicku visinu odredio formation scheduler.

Za provjeru stanja:

```bash
sqlite3 central_server.db
```

```sql
.headers on
.mode column
SELECT mission_id, mission_type, route_id, altitude, altitude_slot, status, queue_reason
FROM missions
ORDER BY created_at;
```

Kada scheduler otkrije presjek/blizinu, centralni server ispisuje `[CENTRAL][ROUTE] konflikt...`; ako je moguce vertikalno razdvajanje, odmah zatim ispisuje odabranu alternativnu sigurnu visinu.

### Test geometrijskog konflikta

Za jasan test pokreni najmanje cetiri autentifikovana drona. Zatim zauzmi tri visinska slota na `SKENDERIJA_K1` sa tri MONITORING misije na trazenoj visini 120 m. Scheduler ce ih rasporediti na 120, 122 i 124 m. Nakon toga posalji DELIVERY od centra zone prema tacki oko 600 m istocno od centra; ta putanja sijece K1. DELIVERY ce probati 120, 122 i 124 m, detektovati konflikt na sva tri slota i ostati `QUEUED` sa `ROUTE_CONFLICT_NO_SAFE_ALTITUDE`. Kad se jedna od tri monitoring misije zavrsi ili zaustavi, scheduler ponovo pokusava queued misiju i moze iskoristiti oslobodjenu sigurnu visinu.


## RTB i stanje baterije

Povratak u bazu koristi eksplicitna stanja:

```text
RETURN_TO_BASE -> AT_BASE -> AVAILABLE
```

Kod alarma `LOW_BATTERY` tok je:

```text
RETURN_TO_BASE -> AT_BASE_LOW_BATTERY -> CHARGING -> AT_BASE -> AVAILABLE
```

U simulatoru je geografski povratak u bazu trenutan, ali stanje povratka i potvrda
dolaska se protokolski evidentiraju. Low-battery dron se nakon dolaska ne vraća
odmah scheduleru. Simulirano punjenje povećava bateriju za 10% svake 2 sekunde,
a `DRONE_READY` se šalje tek na 80%. Centralni server dodatno odbija
`DRONE_READY` zahtjev ispod tog praga.



Za brzi test `LOW_BATTERY` scenarija mogu se koristiti samo simulacijske
environment varijable (normalni default ostaje 100% i pad baterije svake 120 s):

```bash
SDP_INITIAL_BATTERY=21 SDP_BATTERY_TICK_SECONDS=2 \
./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120
```

Dron se registruje sa 21%, nakon približno 2 s pada na 20%, šalje
`LOW_BATTERY`, izvršava RTB i ulazi u `CHARGING`. Zatim se u demo režimu puni
10% svake 2 s do 80%, nakon čega traži `AVAILABLE` od centralnog servera.

