# Geo-rute / konture za autonomne dronove

Ova verzija uvodi:
- zone sa geografskim centrom i radijusom,
- konture kao rute oko centra zone,
- dijagonalni connector kao prelaz između kontura,
- maksimalno 3 drona po konturi,
- visinske slotove razmaknute po 2m,
- kretanje drona po LAT/LON waypoint tačkama.

## Kompajliranje

Ako je `include` folder u istom direktoriju:

```bash
g++ -std=c++11 -I include central_server_geo_routes.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I include regional_server_geo_routes.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I include drone_client_geo_routes.cpp -o drone_client -lboost_system -pthread
```

Ako je `include` folder jedan nivo iznad:

```bash
g++ -std=c++11 -I ../include central_server_geo_routes.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I ../include regional_server_geo_routes.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I ../include drone_client_geo_routes.cpp -o drone_client -lboost_system -pthread
```

## Pokretanje

Obriši stare baze ako testiraš novu strukturu:

```bash
rm -f central_server.db REGION_SARAJEVO_regional_server.db
```

Terminal 1:

```bash
./central_server 9000
```

Terminal 2:

```bash
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 BASCARSIJA:43.8590:18.4310:800:4,SKENDERIJA:43.8563:18.4131:1000:4
```

Format zone:

```text
ZONE_ID:CENTER_LAT:CENTER_LON:RADIUS_M:NUMBER_OF_CONTOURS
```

Centralni server automatski pravi rute:
- SKENDERIJA_K1
- SKENDERIJA_K2
- SKENDERIJA_K3
- SKENDERIJA_K4
- SKENDERIJA_DIAGONAL

## Pokretanje dronova

Tri drona na istoj konturi. Centralni im dodjeljuje visine 120m, 122m, 124m:

```bash
./drone_client 127.0.0.1 8000 DRON_001 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
./drone_client 127.0.0.1 8000 DRON_002 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
./drone_client 127.0.0.1 8000 DRON_003 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
```

Četvrti dron na istoj konturi treba biti odbijen:

```bash
./drone_client 127.0.0.1 8000 DRON_004 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120
```

Očekivani razlog:

```json
{"TYPE":"MISSION_REJECTED","REASON":"ROUTE_CAPACITY_FULL"}
```

Dron na drugoj konturi može proći:

```bash
./drone_client 127.0.0.1 8000 DRON_005 abc123 SKENDERIJA MONITORING SKENDERIJA_K3 120
```

Testni let bez ručnog `route_id` koristi automatski prvu konturu:

```bash
./drone_client 127.0.0.1 8000 DRON_TEST abc123 SKENDERIJA TEST_FLIGHT
```

## Provjera baze

```bash
sqlite3 central_server.db
SELECT * FROM zones;
SELECT * FROM zone_routes;
SELECT * FROM missions;
SELECT * FROM drones;
```

## Logika

Zona se definiše centralnom geografskom tačkom i radijusom. Centralni server iz toga generiše konture kao rute. Dron nakon odobrenja misije dobija centar zone, radijus konture i dodijeljenu visinu. Na osnovu toga generiše waypoint tačke i u telemetriji mijenja svoje LAT/LON koordinate.
