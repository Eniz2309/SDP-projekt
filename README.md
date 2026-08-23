# SDP-projekt

Projekt iz predmeta **Softverski dizajn protokola** na Elektrotehničkom fakultetu Univerziteta u Sarajevu.

## Autonomni dronovi

Projekt predstavlja dizajn i implementaciju mrežnog protokola za upravljanje sistemom autonomnih dronova.

Sistem se sastoji od centralnog servera, regionalnog servera i klijentskih aplikacija dronova. Dronovi komuniciraju sa pripadajućim regionalnim serverom, dok centralni server omogućava globalni nadzor, upravljanje misijama i planiranje ruta.

Direktna peer-to-peer komunikacija između dronova nije dozvoljena.

## Funkcionalnosti

Implementirane su sljedeće funkcionalnosti:

- registracija dronova pomoću URI identifikatora,
- autentifikacija korištenjem sigurnosnog tokena,
- periodično slanje TELEMETRY i KEEPALIVE poruka,
- kreiranje i dodjela misija,
- prioritizacija misija,
- pokretanje i zaustavljanje misija,
- promjena parametara leta,
- inspekcijske misije i praćenje kontura,
- formacijski let,
- planiranje i nadzor ruta,
- detekcija konflikata između ruta,
- alarmni sistem,
- detekcija prekida komunikacije,
- hitan povratak drona u bazu (RTB),
- SQLite baze podataka,
- TLS zaštićena komunikacija uz podršku za PQC mehanizme.

## Komponente

Glavne komponente projekta su:

```text
central_server.cpp
regional_server.cpp
drone_client.cpp
mission_client.cpp
```

- `central_server.cpp` – centralni nadzor, upravljanje misijama i planiranje ruta
- `regional_server.cpp` – komunikacija sa dronovima i posredovanje prema centralnom serveru
- `drone_client.cpp` – simulacija autonomnog drona
- `mission_client.cpp` – kreiranje i slanje zahtjeva za misije

## Komunikacija

Za komunikaciju se koriste TCP i UDP protokoli.

**TCP** se koristi za signalizacijske i upravljačke poruke, dok se **UDP** koristi za periodično slanje:

```text
TELEMETRY
KEEPALIVE
```

Za zaštitu kritične TCP komunikacije koristi se **TLS/OpenSSL** uz podršku za post-kvantne kriptografske mehanizme.

## Kompajliranje

```bash
chmod +x build.sh
./build.sh
```

Nakon uspješnog kompajliranja generišu se:

```text
central_server
regional_server
drone_client
mission_client
```

## Pokretanje

Komponente se pokreću redoslijedom:

```bash
./central_server
./regional_server
./drone_client
```

Za slanje misija koristi se:

```bash
./mission_client
```

Prije pokretanja komunikacije na različitim mašinama potrebno je razmijeniti i sinhronizovati odgovarajuće certifikate.

Testnim skriptama provjerene su glavne funkcionalnosti protokola, uključujući registraciju, autentifikaciju, telemetriju, misije, prioritete, watchdog, alarme, RTB, formacijski let i sigurnu komunikaciju.

## Autori

**Eniz Balihodžić**  
**Merjema Varupa**

Elektrotehnički fakultet Univerziteta u Sarajevu  
Odsjek za telekomunikacije  

Predmet: **Softverski dizajn protokola**  
Profesor: **Doc. dr. Miralem Mehić**
