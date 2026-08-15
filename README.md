# SDP-projekt

## Protokol za upravljanje sistemom autonomnih dronova

Projekt iz predmeta **Softverski dizajn protokola** na Elektrotehničkom fakultetu Univerziteta u Sarajevu.

Cilj projekta je razvoj vlastitog aplikacijskog mrežnog protokola za upravljanje sistemom autonomnih dronova.

Protokol **nije baziran na HTTP protokolu** i ne predstavlja REST API. Komunikacija je realizovana direktno korištenjem **TCP i UDP transportnih protokola** pomoću biblioteke **Boost.Asio**.

---

## Arhitektura sistema

Sistem je organizovan hijerarhijski:

```text
                     +----------------+
                     | CENTRAL SERVER |
                     +----------------+
                              ^
                              |
                             TCP
                              |
                     +-----------------+
                     | REGIONAL SERVER |
                     +-----------------+
                         ^         ^
                         |         |
                        TCP       UDP
                         |         |
                     +-----------------+
                     |      DRON       |
                     +-----------------+
```

Sistem se sastoji od tri glavne komponente:

### Centralni server

Centralni server održava globalno stanje sistema i zadužen je za:

* registraciju regionalnih servera;
* čuvanje zona i ruta;
* planiranje i odobravanje misija;
* dodjelu visinskih slotova;
* sprečavanje konflikta ruta;
* obradu prioriteta misija;
* obradu alarma;
* čuvanje globalnog stanja dronova i misija.

### Regionalni server

Regionalni server predstavlja posrednika između dronova i centralnog servera.

Zadužen je za:

* registraciju i autentifikaciju dronova;
* lokalno čuvanje statusa dronova;
* prijem telemetrije i keep-alive poruka;
* validaciju zona;
* prosljeđivanje zahtjeva centralnom serveru;
* prosljeđivanje komandi dronu.

### Dron

Dron predstavlja klijentsku aplikaciju koja simulira autonomni dron.

Dron:

* registruje se pomoću URI identifikatora;
* autentifikuje se sigurnosnim tokenom;
* periodično šalje telemetriju;
* šalje keep-alive poruke;
* zahtijeva pokretanje misije;
* izvršava dodijeljenu rutu;
* prijavljuje završetak misije;
* prati stanje baterije;
* šalje alarm pri niskom nivou baterije;
* izvršava komandu za povratak u bazu.

Dronovi **ne komuniciraju direktno međusobno niti direktno sa centralnim serverom**.

---

# TCP i UDP komunikacija

Protokol koristi oba transportna protokola.

## TCP

TCP se koristi za kontrolne i signalizacijske poruke kod kojih je potrebna pouzdana i uređena isporuka.

Primjeri TCP poruka:

```text
REGISTER_REQ
REGISTER_ACK

AUTH_REQ
AUTH_ACK

MISSION_REQUEST
MISSION_APPROVED
MISSION_REJECTED
MISSION_FINISHED

ALARM
RETURN_TO_BASE
ACK_RTB

STOP_MISSION
CHANGE_PARAMS
ACK poruke
```

TCP komunikacija predstavlja **byte-stream komunikaciju**.

Poruke su kodirane u JSON formatu, a granica između TCP poruka određena je znakom novog reda:

```text
JSON + '\n'
```

## UDP

UDP se koristi za periodične podatke kod kojih je važnija mala latencija od retransmisije zastarjelog podatka.

Preko UDP-a se šalju:

```text
TELEMETRY
KEEPALIVE
```

Svaki UDP datagram sadrži jedan JSON objekat:

```text
1 UDP datagram = 1 protokolska poruka
```

Za UDP telemetriju ne koristi se ACK niti retransmisija.

Ako se izgubi jedna telemetrijska poruka, naredna poruka uskoro donosi novije stanje drona.

---

# Osnovne poruke protokola

| Poruka             | Smjer                         | Transport | Namjena                    |
| ------------------ | ----------------------------- | --------- | -------------------------- |
| `REGISTER_REQ`     | Dron → Regionalni             | TCP       | Registracija drona         |
| `REGISTER_ACK`     | Regionalni → Dron             | TCP       | Potvrda registracije       |
| `AUTH_REQ`         | Dron → Regionalni             | TCP       | Autentifikacija            |
| `AUTH_ACK`         | Regionalni → Dron             | TCP       | Potvrda autentifikacije    |
| `KEEPALIVE`        | Dron → Regionalni             | UDP       | Provjera dostupnosti drona |
| `TELEMETRY`        | Dron → Regionalni             | UDP       | Periodično stanje drona    |
| `DRONE_STATUS`     | Regionalni → Centralni        | TCP       | Ažuriranje statusa drona   |
| `MISSION_REQUEST`  | Dron → Regionalni → Centralni | TCP       | Zahtjev za misiju          |
| `MISSION_APPROVED` | Centralni → Regionalni → Dron | TCP       | Odobrena misija            |
| `MISSION_REJECTED` | Centralni → Regionalni → Dron | TCP       | Odbijena misija            |
| `MISSION_FINISHED` | Dron → Regionalni → Centralni | TCP       | Završetak misije           |
| `ALARM`            | Dron → Regionalni → Centralni | TCP       | Alarmno stanje             |
| `RETURN_TO_BASE`   | Centralni → Regionalni → Dron | TCP       | Hitan povratak u bazu      |
| `ACK_RTB`          | Dron → Regionalni             | TCP       | Potvrda RTB komande        |

---

# Misije

Trenutna implementacija podržava:

```text
TEST_FLIGHT
MONITORING
DELIVERY
```

Definisani su i prioriteti misija:

| Tip misije    | Prioritet |
| ------------- | --------: |
| `MONITORING`  |         4 |
| `DELIVERY`    |         3 |
| `INSPECTION`  |         2 |
| `TEST_FLIGHT` |         1 |

Veći broj označava veći prioritet.

Ako je ruta popunjena, misija višeg prioriteta može preuzeti slot aktivne misije nižeg prioriteta.

---

# Zone i rute

Regionalni server se pokreće sa jednom ili više geografskih zona.

Format zone:

```text
ZONE_ID:CENTER_LAT:CENTER_LON:RADIUS_M:BROJ_KONTURA
```

Primjer:

```text
SKENDERIJA:43.8563:18.4131:1000:4
```

Za navedenu zonu centralni server generiše više kontura:

```text
SKENDERIJA_K1
SKENDERIJA_K2
SKENDERIJA_K3
SKENDERIJA_K4
```

Na istoj ruti trenutno mogu biti maksimalno **3 drona**, pri čemu se koriste različiti visinski slotovi sa vertikalnom separacijom od **2 m**.

Primjer:

```text
120 m
122 m
124 m
```

---

# DELIVERY misija

Kod `DELIVERY` misije centralni server na osnovu zadane GPS koordinate:

1. određuje položaj dostavne tačke;
2. bira odgovarajuću konturu;
3. računa najbližu izlaznu tačku sa konture;
4. dodjeljuje rutu i visinski slot;
5. šalje dronu dostavnu i izlaznu koordinatu.

Dron se kreće po dodijeljenoj konturi, napušta je na izračunatoj tački i nastavlja prema dostavnoj lokaciji.

Nakon završetka šalje:

```text
MISSION_FINISHED
```

---

# Baterija i RETURN_TO_BASE

Simulacija baterije koristi odnos:

```text
1% baterije = 120 sekundi
```

Kada baterija padne na 20% ili manje, dron šalje:

```text
ALARM
ALARM_TYPE = LOW_BATTERY
```

Tok poruka:

```text
DRON
 |
 | ALARM / LOW_BATTERY
 v
REGIONALNI SERVER
 |
 v
CENTRALNI SERVER
 |
 | RETURN_TO_BASE
 v
REGIONALNI SERVER
 |
 v
DRON
```

Dron zatim prelazi u stanje:

```text
RETURN_TO_BASE
```

i vraća se na koordinate baze regionalnog servera.

---

# Struktura repozitorija

```text
SDP-projekt/
│
├── central_server.cpp
├── regional_server.cpp
├── drone_client.cpp
│
├── sqlite3_wrapper.h
│
├── json/
│   └── json.h
│
├── UPPAAL/
│   └── SDP_projekt.xml
│
├── README-ovi/
│
└── README.md
```

---

# Potrebni paketi

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install g++ libboost-system-dev libsqlite3-dev sqlite3
```

Projekt koristi:

* C++11;
* Boost.Asio;
* SQLite3;
* nlohmann JSON;
* UPPAAL.

---

# Kloniranje projekta

```bash
git clone https://github.com/Eniz2309/SDP-projekt.git
cd SDP-projekt
```

---

# Kompajliranje

Centralni server:

```bash
g++ -std=c++11 -I . central_server.cpp \
-o central_server \
-lboost_system -lsqlite3 -pthread
```

Regionalni server:

```bash
g++ -std=c++11 -I . regional_server.cpp \
-o regional_server \
-lboost_system -lsqlite3 -pthread
```

Drone client:

```bash
g++ -std=c++11 -I . drone_client.cpp \
-o drone_client \
-lboost_system -pthread
```

---

# Pokretanje sistema

Sistem se pokreće redoslijedom:

```text
1. Centralni server
2. Regionalni server
3. Jedan ili više dronova
```

## 1. Centralni server

```bash
./central_server 9000
```

---

## 2. Regionalni server

Primjer:

```bash
./regional_server \
REGION_SARAJEVO \
127.0.0.1 \
9000 \
8000 \
8001 \
43.8563 \
18.4131 \
SKENDERIJA:43.8563:18.4131:1000:4,BASCARSIJA:43.8590:18.4310:800:4
```

Argumenti:

```text
REGION_SARAJEVO    ID regionalnog servera
127.0.0.1          IP adresa centralnog servera
9000               TCP port centralnog servera
8000               TCP port za komunikaciju sa dronovima
8001               UDP port za telemetriju i keep-alive
43.8563            latitude baze
18.4131            longitude baze
...                 konfiguracija zona
```

---

## 3. MONITORING dron

```bash
./drone_client \
127.0.0.1 \
8000 \
8001 \
DRON_001 \
abc123 \
SKENDERIJA \
MONITORING \
SKENDERIJA_K2 \
120
```

---

## 4. TEST_FLIGHT

```bash
./drone_client \
127.0.0.1 \
8000 \
8001 \
DRON_TEST \
abc123 \
SKENDERIJA \
TEST_FLIGHT \
SKENDERIJA_K1 \
80
```

---

## 5. DELIVERY

```bash
./drone_client \
127.0.0.1 \
8000 \
8001 \
DRON_DEL \
abc123 \
SKENDERIJA \
DELIVERY \
AUTO \
120 \
43.8580 \
18.4160
```

Kod `AUTO` opcije centralni server automatski bira odgovarajuću rutu.

---

# UDP test

Nakon autentifikacije dron periodično šalje:

```text
TELEMETRY   → svake 3 sekunde
KEEPALIVE   → svakih 15 sekundi
```

Na regionalnom serveru očekuju se poruke poput:

```text
[REGIONAL][UDP] TELEMETRY od DRON_001
[REGIONAL][UDP] KEEPALIVE od DRON_001
```

---

# Wireshark

TCP komunikacija drona i regionalnog servera:

```text
tcp.port == 8000
```

UDP komunikacija:

```text
udp.port == 8001
```

Komunikacija regionalnog i centralnog servera:

```text
tcp.port == 9000
```

---

# SQLite baze

## Centralni server

```bash
sqlite3 central_server.db
```

Primjeri:

```sql
SELECT * FROM regional_servers;
SELECT * FROM zones;
SELECT * FROM zone_routes;
SELECT * FROM drones;
SELECT * FROM missions;
SELECT * FROM alarms;
```

## Regionalni server

```bash
sqlite3 REGION_SARAJEVO_regional_server.db
```

Primjeri:

```sql
SELECT * FROM zones;
SELECT * FROM drones;
SELECT * FROM keepalive_log;
SELECT * FROM alarms;
```

---

# UPPAAL model

Formalni model protokola nalazi se u:

```text
UPPAAL/SDP_projekt.xml
```

Model sadrži automate za:

```text
Drone
RegionalServer
CentralServer
```

UPPAAL se koristi za simulaciju i formalnu verifikaciju ponašanja protokola.

---

# Trenutno implementirano

* Centralni server ↔ Regionalni server ↔ Dron arhitektura
* vlastiti aplikacijski mrežni protokol bez HTTP-a
* TCP komunikacija
* UDP komunikacija
* JSON protokolske poruke
* registracija dronova
* autentifikacija tokenom
* periodični keep-alive
* periodična telemetrija
* SQLite baze
* regionalne zone
* generisanje ruta
* visinski slotovi
* kontrola broja dronova na istoj ruti
* zahtjevi i odobravanje misija
* prioriteti misija
* TEST_FLIGHT
* MONITORING
* DELIVERY
* simulacija baterije
* LOW_BATTERY alarm
* RETURN_TO_BASE
* UPPAAL model protokola

---

# Dalji razvoj

Planirane naredne faze:

* connection-loss watchdog;
* kompletiranje `INSPECTION` scenarija;
* aktivno slanje `STOP_MISSION` preemptovanom dronu;
* kolaborativno/formacijsko letenje;
* TLS zaštita komunikacije;
* post-kvantna kriptografija;
* Boost.Test testovi;
* testne i benchmark skripte;
* Wireshark `.pcap` zapisi;
* završna UPPAAL verifikacija.

---

## Tehnologije

```text
C++11
Boost.Asio
TCP
UDP
JSON
SQLite
UPPAAL
```

> Projekt je trenutno u aktivnom razvoju.
