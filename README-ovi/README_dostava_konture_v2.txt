# Autonomous Drone Protocol - konture i dostava

Ova verzija ima:
- regionalne servere sa zonama;
- zone definisane centrom, radijusom i brojem kontura;
- kockaste/pravougaone konture oko centra zone;
- maksimalno 3 drona po istoj konturi;
- visinske slotove razmaknute po 2m;
- DELIVERY misiju sa dostavnom koordinatom;
- bateriju: 1% baterije traje 2 minute.

## Kompajliranje

Ako je `include` folder u istom direktoriju:

```bash
g++ -std=c++11 -I include central_server.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I include regional_server.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I include drone_client.cpp -o drone_client -lboost_system -pthread
```

Ako je `include` folder jedan nivo iznad:

```bash
g++ -std=c++11 -I ../include central_server.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I ../include regional_server.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I ../include drone_client.cpp -o drone_client -lboost_system -pthread
```

## Čisto pokretanje

```bash
rm -f central_server.db REGION_SARAJEVO_regional_server.db
```

## 1. Centralni server

```bash
./central_server 9000
```

## 2. Regionalni server Sarajevo

```bash
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4,BASCARSIJA:43.8590:18.4310:800:4
```

Format zone:

```text
ZONE_ID:CENTER_LAT:CENTER_LON:RADIUS_M:BROJ_KONTURA
```

Centralni server za `SKENDERIJA:43.8563:18.4131:1000:4` napravi:
- `SKENDERIJA_K1` = 250m od centra
- `SKENDERIJA_K2` = 500m od centra
- `SKENDERIJA_K3` = 750m od centra
- `SKENDERIJA_K4` = 1000m od centra
- `SKENDERIJA_DIAGONAL` = prelazni koridor

## 3. Monitoring test - više dronova na istoj konturi

Tri drona mogu na istu konturu. Dobijaju visine 120m, 122m i 124m:

```bash
./drone_client 127.0.0.1 8000 DRON_001 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
./drone_client 127.0.0.1 8000 DRON_002 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
./drone_client 127.0.0.1 8000 DRON_003 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
```

Četvrti na istoj konturi treba biti odbijen:

```bash
./drone_client 127.0.0.1 8000 DRON_004 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
```

Očekivano:

```json
{"TYPE":"MISSION_REJECTED","REASON":"ROUTE_CAPACITY_FULL"}
```

## 4. Dostava

Kod dostave ne moraš ručno dati konturu. Stavi `AUTO`, a centralni server bira najbližu konturu dostavnoj tački.

```bash
./drone_client 127.0.0.1 8000 DRON_DEL abc123 SKENDERIJA DELIVERY AUTO 120 43.8580 18.4160
```

Argumenti za dostavu:

```text
DRON_DEL      URI drona
abc123        token
SKENDERIJA    zona
DELIVERY      tip misije
AUTO          centralni server bira konturu
120           osnovna visina
43.8580       delivery latitude
18.4160       delivery longitude
```

Centralni server:
1. računa položaj dostavne tačke u odnosu na centar zone;
2. bira najbližu kockastu konturu;
3. računa najbližu tačku na konturi;
4. šalje dronu `EXIT_LAT` i `EXIT_LON`;
5. šalje dronu `DELIVERY_LAT` i `DELIVERY_LON`.

Dron:
1. kreće se po konturi;
2. ide do najbliže tačke na konturi;
3. napušta konturu;
4. prilazi dostavnoj tački pod 90 stepeni;
5. šalje `MISSION_FINISHED`.

## 5. Provjera baze

```bash
sqlite3 central_server.db
SELECT * FROM regional_servers;
SELECT * FROM zones;
SELECT * FROM zone_routes;
SELECT * FROM missions;
SELECT * FROM drones;
SELECT * FROM alarms;
```

Regionalna baza:

```bash
sqlite3 REGION_SARAJEVO_regional_server.db
SELECT * FROM zones;
SELECT * FROM drones;
SELECT * FROM keepalive_log;
SELECT * FROM alarms;
```

## Baterija

Baterija se smanjuje pomoću posebnog tajmera u `drone_client.cpp`.

```text
1% baterije = 120 sekundi = 2 minute
```

Znači baterija se ne smanjuje na svakoj telemetriji, nego jednom u 2 minute dok je dron aktivan.


## 5. Testni let

Testni let se pokreće tako što se kao tip misije navede `TEST_FLIGHT`.

Najjednostavnije pokretanje:

```bash
./drone_client 127.0.0.1 8000 DRON_TEST abc123 SKENDERIJA TEST_FLIGHT
```

U ovoj varijanti dron šalje `ROUTE_ID = AUTO`, a centralni server automatski bira prvu konturu zone:

```text
SKENDERIJA_K1
```

To znači da testni let ide po najbližoj/najmanjoj konturi oko centra zone.

Ako želiš ručno zadati konturu i visinu, možeš ovako:

```bash
./drone_client 127.0.0.1 8000 DRON_TEST abc123 SKENDERIJA TEST_FLIGHT SKENDERIJA_K1 80
```

Argumenti su:

```text
127.0.0.1       adresa regionalnog servera
8000            port regionalnog servera
DRON_TEST       URI identifikator drona
abc123          sigurnosni token
SKENDERIJA      zona
TEST_FLIGHT     tip misije
SKENDERIJA_K1   kontura/ruta za testni let
80              osnovna visina leta
```

Kod testnog leta dron napravi jedan krug po zadanoj konturi i nakon toga šalje:

```json
{
  "TYPE": "MISSION_FINISHED",
  "MISSION_ID": "DRON_TEST_M001",
  "DRONE_URI": "DRON_TEST"
}
```

Centralni server tada postavlja misiju kao `FINISHED`, čime se ruta i visinski slot oslobađaju.
