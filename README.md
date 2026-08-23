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
- prekid cijele formacije ako bilo koji clan prijavi LOW_BATTERY ili izgubi vezu
- STOP_MISSION, promjena parametara leta i rucni RTB
- LOW_BATTERY, SIGNAL_LOSS i CONNECTION_LOST alarmi
- UDP TELEMETRY i KEEPALIVE sa zadatkom, rezimom leta i statusom senzora
- SQLite evidencija dronova, misija, alarma, zona i inspection izvjestaja
- TLS 1.3 sa `X25519MLKEM768` i `ML-DSA-44`
- AES-256-GCM zastita UDP payload-a kljucem izvedenim iz TLS sesije
- UDP anti-replay zastita monotonim `SEQ` brojem po TLS sesiji
- lokalni failsafe RTB kada drone client izgubi TCP/TLS vezu sa regionalnim serverom
- kontinuirani live nadzor odstupanja od planirane rute i stvarne separacije dronova

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

## Formation failure handling

Formacija se tretira kao jedna koordinisana misija. Ako bilo koji clan dobije `LOW_BATTERY` ili `CONNECTION_LOST`, centralni server pronalazi njegovu aktivnu formaciju preko `formation_members`, oznacava formaciju prekinutom i regionalnom serveru salje `STOP_MISSION` za sve preostale clanove.

Kod `LOW_BATTERY` neispravni clan dobija `RETURN_TO_BASE`, a ostali clanovi postaju raspolozivi tek nakon sto potvrde `ACK_STOP`. Kod `CONNECTION_LOST` izgubljenom clanu se ne pokusava slati komanda preko nepostojece veze; ostali clanovi se zaustavljaju i virtualni leader se gasi.

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
SIGNAL_STRENGTH
SEQ
```

`SIGNAL_STRENGTH` je simulirana jacina radio veze od 0 do 100%. `SEQ` je
monotono rastuci broj UDP poruke i nalazi se unutar AES-GCM autentificiranog
payload-a.

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

UDP TELEMETRY/KEEPALIVE se salju kao AES-256-GCM envelope sa `NONCE`, `TAG` i `CIPHERTEXT` poljima. Svaki enkriptovani payload sadrzi i monoton `SEQ`. Regionalni server pamti posljednji prihvaceni `SEQ` za aktivnu TLS sesiju i odbacuje svaki datagram sa `SEQ <= last_seq`, cime se sprecava replay vec snimljene validne telemetrije ili keepalive poruke.

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



## SIGNAL_LOSS

Pored prekida TCP/TLS veze, protokol modelira i degradaciju radio signala.
`SIGNAL_STRENGTH` se salje u telemetriji. Normalni default je 100% i signal se
sam od sebe ne smanjuje. Za demo test moze se ukljuciti simulirani pad:

```bash
SDP_INITIAL_SIGNAL=25 SDP_SIGNAL_TICK_SECONDS=2 SDP_SIGNAL_DROP_PER_TICK=3 \
./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120
```

Tok je:

```text
SIGNAL_STRENGTH <= 20%
        -> SIGNAL_LOSS alarm
        -> centralni prekida aktivnu misiju
        -> RETURN_TO_BASE
        -> AT_BASE
        -> signal se na bazi simulacijski oporavlja na 100%
        -> AVAILABLE
```

Ako je dron clan formacije, `SIGNAL_LOSS` jednog clana prekida cijelu
formaciju, failed clan dobija RTB, a ostali clanovi `STOP_MISSION`.

## Lokalni failsafe kod prekida veze

Drone client iz `AUTH_ACK` pamti koordinate pripadajuce regionalne baze. Ako
tokom leta TCP/TLS veza sa regionalnim serverom stvarno pukne, dron ne moze
cekati server komandu. Lokalni FSM zato odmah izvrsava:

```text
TCP/TLS DISCONNECT
      -> CONNECTION_LOST_LOCAL_FAILSAFE
      -> RETURN_TO_BASE
      -> AT_BASE_CONNECTION_LOST
```

U simulatoru je geografski povratak trenutan. Regionalni watchdog nezavisno
detektuje izostanak telemetrije/keepalive poruka i globalno prijavljuje
`CONNECTION_LOST` centralnom serveru.

## Live nadzor rute

Planiranje konflikta se radi prije dodjele, ali centralni server dodatno na
svakoj telemetriji nadzire stvarno izvrsavanje aktivne misije.

Provjeravaju se:

- udaljenost trenutne pozicije od planirane putanje;
- stvarna horizontalna i vertikalna separacija od drugih aktivnih dronova;
- clanovi iste formacije se izuzimaju iz medjusobne conflict provjere jer su
  namjerno koordinisani na istoj visini.

Ako dron izadje iz dozvoljenog koridora, u tabelu `alarms` upisuje se
`ROUTE_DEVIATION`. Ako dvije nezavisne aktivne letjelice u stvarnom vremenu
prekrse sigurnu separaciju, upisuje se `LIVE_ROUTE_CONFLICT`.

Provjera:

```sql
SELECT id, drone_uri, alarm_type, message, created_at
FROM alarms
WHERE alarm_type IN ('SIGNAL_LOSS', 'ROUTE_DEVIATION', 'LIVE_ROUTE_CONFLICT')
ORDER BY id DESC;
```

## Anti-replay provjera

Normalan regionalni log za UDP izgleda ovako:

```text
[REGIONAL][UDP][AES-256-GCM] TELEMETRY od DRON_001 | SEQ=15 | ...
```

Ako se isti validni datagram ponovi, regionalni ga ne prosljedjuje centralnom:

```text
[REGIONAL][UDP][ANTI-REPLAY] Odbijen DRON_001 SEQ=15 last=15
```


# Instalacija OpenSSL 3.5.7 sa PQC podrškom

Projekt koristi OpenSSL 3.5.7 zbog podrške za post-kvantne algoritme kao što su:

- ML-DSA-44
- ML-KEM
- X25519MLKEM768

Sistemski OpenSSL na Ubuntu 22.04 se ne uklanja. Nova verzija se instalira odvojeno u:

/usr/local/openssl

## 1. Instalacija potrebnih paketa

sudo apt update
sudo apt install -y \
build-essential \
gcc \
g++ \
make \
perl \
wget \
ca-certificates

## 2. Preuzimanje OpenSSL 3.5.7

cd /tmp
wget https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz

Raspakovati arhivu:

tar -xzf openssl-3.5.7.tar.gz

Ući u direktorij:

cd openssl-3.5.7

## 3. Konfiguracija

./Configure \
--prefix=/usr/local/openssl \
--openssldir=/usr/local/openssl/ssl \
shared

## 4. Kompajliranje

make -j"$(nproc)"

## 5. Instalacija

sudo make install_sw

## 6. Dodavanje openssl.cnf

Nakon make install_sw konfiguracioni fajl može nedostajati u /usr/local/openssl/ssl.

Kreirati direktorij:

sudo mkdir -p /usr/local/openssl/ssl

Kopirati konfiguracioni fajl:

sudo cp /tmp/openssl-3.5.7/apps/openssl.cnf \
/usr/local/openssl/ssl/openssl.cnf

Provjera:

ls -l /usr/local/openssl/ssl/openssl.cnf

## 7. Podešavanje okruženja

Za trenutni terminal:

export PATH=/usr/local/openssl/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/openssl/lib64:/usr/local/openssl/lib:${LD_LIBRARY_PATH:-}

Provjeriti koji OpenSSL se koristi:

which openssl

Očekivano:

/usr/local/openssl/bin/openssl

Provjeriti verziju:

openssl version

Očekivano:

OpenSSL 3.5.7 ...

## 8. Trajno podešavanje

Da nije potrebno izvršavati export komande nakon svakog otvaranja terminala, dodati ih u ~/.bashrc:

echo 'export PATH=/usr/local/openssl/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/openssl/lib64:/usr/local/openssl/lib:${LD_LIBRARY_PATH:-}' >> ~/.bashrc

Učitati novu konfiguraciju:

source ~/.bashrc

## 9. Provjera instalacije

Detaljna provjera verzije:

openssl version -a

Provjera konfiguracionog direktorija:

openssl version -d

Očekivano:

OPENSSLDIR: "/usr/local/openssl/ssl"

Provjera biblioteka koje koristi novi OpenSSL:

ldd /usr/local/openssl/bin/openssl | grep -E 'ssl|crypto'

Biblioteke trebaju dolaziti iz:

/usr/local/openssl/lib64/

ili:

/usr/local/openssl/lib/

## 10. Provjera PQC algoritama

Provjera ML-DSA:

openssl list -signature-algorithms | grep -i ML-DSA

Trebao bi biti dostupan najmanje:

ML-DSA-44

Provjera ML-KEM:

openssl list -kem-algorithms | grep -i ML-KEM

Provjera TLS grupa:

openssl list -tls-groups | grep -Ei 'MLKEM|X25519MLKEM'

Za projekt se koristi:

X25519MLKEM768

Ako su dostupni ML-DSA-44, ML-KEM i X25519MLKEM768, OpenSSL instalacija je spremna za korištenje u projektu.
