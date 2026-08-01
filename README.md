# SDP-projekt
Repozitorij za projekt iz predmeta Softverski Dizajn Protokola - protokol za upravljanje sistemom autonomnih dronova



# Autonomous Drone Control Protocol

Projekat predstavlja implementaciju protokola za upravljanje autonomnim dronovima. Sistem se sastoji od centralnog servera, regionalnih servera i klijentskih aplikacija dronova.

## Struktura projekta

```text
.
├── central_server.cpp
├── regional_server.cpp
├── drone_client.cpp
├── json/
│    └── json.h
│-- sqlite3_wrapper.h
└── README.md
```

## Kloniranje projekta
git clone https://www.github.com/Eniz2309/SDP-projekt


## Potrebne biblioteke

Na Ubuntu/Debian sistemu instalirati:

sudo apt update
sudo apt install g++ libboost-system-dev libsqlite3-dev


Projekat koristi:

- Boost.Asio za TCP komunikaciju
- JSON biblioteku za format poruka
- SQLite za lokalno čuvanje podataka

## Kompajliranje

Ako se `include` folder nalazi u istom direktoriju kao `.cpp` fajlovi, kompajliranje ide ovako:

g++ -std=c++11 -I include central_server.cpp -o central_server -lboost_system -lsqlite3 -pthread

g++ -std=c++11 -I include regional_server.cpp -o regional_server -lboost_system -lsqlite3 -pthread

g++ -std=c++11 -I include drone_client.cpp -o drone_client -lboost_system -pthread


## Pokretanje sistema

Prije pokretanja, ako postoje stare baze iz prethodnog testiranja, mogu se obrisati:


rm -f central_server.db regional_server.db

### 1. Pokretanje centralnog servera

U prvom terminalu:

./central_server 9000

Centralni server sluša regionalne servere na portu `9000`.

### 2. Pokretanje regionalnog servera

U drugom terminalu, primjer za Sarajevo:

./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 BASCARSIJA,SKENDERIJA,KOSEVSKO_BRDO,POFALICI,OTOKA

Argumenti su:

REGION_SARAJEVO                         naziv regionalnog servera
127.0.0.1                               adresa centralnog servera
9000                                    port centralnog servera
8000                                    port na kojem regionalni server sluša dronove
43.8563                                 latitude baze regionalnog servera
18.4131                                 longitude baze regionalnog servera
BASCARSIJA,SKENDERIJA,...               zone kojima regionalni server upravlja

Primjer za drugi regionalni server, Tuzla:

./regional_server REGION_TUZLA 127.0.0.1 9000 8001 43.5 18.2 TUZLA,ZIVINICE,KALESIJA,SREBRENIK,GRACANICA

Napomena: ako se više regionalnih servera pokreće na istoj mašini, svaki mora imati različit port za dronove, npr. `8000`, `8001`, `8002`.

### 3. Pokretanje drona

U trećem terminalu:

./drone_client 127.0.0.1 8000 DRON_001 abc123 BASCARSIJA MONITORING

Argumenti su:

127.0.0.1       adresa regionalnog servera
8000            port regionalnog servera
DRON_001        jedinstveni URI identifikator drona
abc123          sigurnosni token
BASCARSIJA      zona u kojoj dron traži misiju
MONITORING      tip misije

Primjeri više dronova:

./drone_client 127.0.0.1 8000 DRON_002 abc123 SKENDERIJA DELIVERY

./drone_client 127.0.0.1 8001 DRON_TZ_001 abc123 TUZLA MONITORING


## Primjer testiranja

Pokrenuti redom:


./central_server 9000

./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 BASCARSIJA,SKENDERIJA,KOSEVSKO_BRDO,POFALICI,OTOKA

./drone_client 127.0.0.1 8000 DRON_001 abc123 BASCARSIJA MONITORING


Ako je sve ispravno, dron će se povezati na regionalni server, poslati registracioni zahtjev, autentifikovati se i početi slati keep-alive i telemetrijske poruke.

## Provjera SQLite baze

Centralna baza:


sqlite3 central_server.db

Primjeri upita:

SELECT * FROM regional_servers;
SELECT * FROM regional_zones;
SELECT * FROM drones;
SELECT * FROM missions;
SELECT * FROM alarms;


Regionalna baza:


sqlite3 regional_server.db


Primjeri upita:


SELECT * FROM regional_zones;
SELECT * FROM drones;
SELECT * FROM keepalive_log;
SELECT * FROM alarms;


## Funkcionalnosti

Implementirane funkcionalnosti uključuju:

- registraciju regionalnih servera
- definisanje zona po regionalnom serveru
- definisanje koordinata baze po regionalnom serveru
- registraciju dronova pomoću URI identifikatora
- autentifikaciju pomoću tokena
- periodično slanje keep-alive i telemetrijskih poruka
- lokalno i centralno čuvanje statusa dronova
- dodjelu misija dronovima
- validaciju zone misije
- detekciju konflikta ruta
- alarm za nizak nivo baterije
- komunikaciju između drona, regionalnog servera i centralnog servera putem JSON poruka
