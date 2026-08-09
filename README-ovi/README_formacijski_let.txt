# Formacijski let - testiranje

Ova verzija dodaje formacijski let kroz multicast poruku sa regionalnog servera prema odabranoj grupi dronova.

## Kompajliranje

Ako je `include` folder u istom direktoriju kao cpp fajlovi:

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

## Pokretanje

Terminal 1:

```bash
./central_server 9000
```

Terminal 2:

```bash
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 BASCARSIJA,SKENDERIJA,KOSEVSKO_BRDO,POFALICI,OTOKA
```

Terminal 3:

```bash
./drone_client 127.0.0.1 8000 DRON_001 abc123 BASCARSIJA MONITORING
```

Terminal 4:

```bash
./drone_client 127.0.0.1 8000 DRON_002 abc123 BASCARSIJA MONITORING
```

Terminal 5:

```bash
./drone_client 127.0.0.1 8000 DRON_003 abc123 BASCARSIJA MONITORING
```

## Komande u terminalu regionalnog servera

Prikaži aktivne dronove:

```text
list_drones
```

Kreiraj formaciju:

```text
formation_create FORMATION_1 DRON_001 DRON_002 DRON_003
```

Pokreni formacijski let:

```text
formation_start FORMATION_1 DRON_001 120 15 EAST 30
```

Zaustavi formaciju:

```text
formation_stop FORMATION_1
```

Dronovi koji su članovi formacije trebaju ispisati `Entering formation FORMATION_1`, preći u `FORMATION_MODE` i poslati `ACK_FORMATION_START` regionalnom serveru.
