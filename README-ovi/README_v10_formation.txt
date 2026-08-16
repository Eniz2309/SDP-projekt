# v10_formation - server-controlled formacijski let

Osnova: v9_scheduler + ispravka regionalnog \"\n\" delimiter buga.

## Koncept
- NEMA drone-leadera.
- REGIONAL_SERVER je FORMATION_CONTROLLER / VIRTUAL_LEADER.
- Centralni scheduler bira 2-5 AVAILABLE dronova.
- Cijela formacija zauzima jedan logicki route/altitude slot.
- SVI clanovi koriste isti ALTITUDE.
- Razdvajanje je horizontalno preko OFFSET_NORTH_M / OFFSET_EAST_M.
- Regionalni server svake 2 sekunde pomjera virtual leader po konturi i salje FORMATION_UPDATE svakom clanu.
- Formacija traje dok operator ne posalje STOP.

## Build
```bash
g++ -std=c++11 -I . dostava_central_server_v10_formation.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I . dostava_regional_server_v10_formation.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I . dostava_drone_client_v10_formation.cpp -o drone_client -lboost_system -pthread
g++ -std=c++11 -I . mission_client_v10_formation.cpp -o mission_client -lboost_system -pthread
```

## Pokretanje
Central:
```bash
./central_server 9000
```

Regional:
```bash
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4
```

Tri drona:
```bash
./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120
./drone_client 127.0.0.1 8000 8001 DRON_002 abc123 120
./drone_client 127.0.0.1 8000 8001 DRON_003 abc123 120
```

Formacija: 3 drona, ista visina 120 m, horizontalni razmak 10 m:
```bash
./mission_client 127.0.0.1 8000 M_FORM FORMATION SKENDERIJA SKENDERIJA_K2 120 3 10
```

Ocekivano:
- CENTRAL: [CENTRAL][FORMATION] ... 3 dronova ... zajednicka visina=120m
- REGIONAL: START_FORMATION za sva 3 + VIRTUAL_LEADER update-i
- DRONE: status FORMATION, isti alt=120, razliciti horizontalni offseti

Stop cijele formacije:
```bash
./mission_client 127.0.0.1 8000 STOP M_FORM
```
Regionalni zaustavlja virtual leader i salje STOP_MISSION svim clanovima. Svaki vraca ACK_STOP; tek kad su svi clanovi STOPPED, centralni oznacava cijelu formaciju STOPPED i ponovo pokrece scheduler.

## Napomena o prioritetu
Osnovni prioriteti ostaju:
MONITORING=4 > DELIVERY=3 > INSPECTION=2 > TEST_FLIGHT=1.
FORMATION je poseban serverski kontrolisan zadatak i u ovoj demo verziji koristi priority=2 radi uklapanja u postojeci scheduler.
