AUTONOMNI DRONOVI - v6_watchdog
=================================

Ova verzija nadogradjuje v5_tcp_udp funkcionalnoscu detekcije gubitka veze sa dronom.

NOVO U v6
---------
- Regionalni server prati posljednju poruku svakog drona.
- TELEMETRY se salje UDP-om svake 3 sekunde.
- KEEPALIVE se salje UDP-om svakih 15 sekundi.
- Watchdog provjerava stanje svakih 5 sekundi.
- Ako 45 sekundi ne stigne TELEMETRY ili KEEPALIVE:
    1. regionalni status drona postaje CONNECTION_LOST;
    2. regionalni upisuje CONNECTION_LOST alarm u lokalnu bazu;
    3. regionalni salje DRONE_STATUS=CONNECTION_LOST centralnom;
    4. regionalni salje ALARM/CONNECTION_LOST centralnom;
    5. centralni status drona postavlja na CONNECTION_LOST;
    6. aktivna misija drona postaje ABORTED_CONNECTION_LOST;
    7. ruta/visinski slot se oslobadjaju jer misija vise nije ACTIVE.

Zasto 45 sekundi?
-----------------
KEEPALIVE interval je 15 sekundi. Timeout od 45 sekundi predstavlja tri keepalive intervala,
tako da jedan ili dva izgubljena UDP datagrama ne izazovu lazni CONNECTION_LOST.

KOMPAJLIRANJE
-------------
Pretpostavka: json/json.h i sqlite3_wrapper.h nalaze se kao i u prethodnoj verziji projekta.

Centralni server:
  g++ -std=c++11 -I . dostava_central_server_v6_watchdog.cpp -o central_server -lboost_system -lsqlite3 -pthread

Regionalni server:
  g++ -std=c++11 -I . dostava_regional_server_v6_watchdog.cpp -o regional_server -lboost_system -lsqlite3 -pthread

Drone client:
  g++ -std=c++11 -I . dostava_drone_client_v6_watchdog.cpp -o drone_client -lboost_system -pthread

POKRETANJE
----------
1) Centralni:
  ./central_server 9000

2) Regionalni:
  ./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4

3) Dron:
  ./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120

TEST WATCHDOGA
--------------
1. Pokreni centralni, regionalni i dron.
2. Provjeri da regionalni prima [UDP] TELEMETRY i KEEPALIVE.
3. Ugasi samo drone_client sa Ctrl+C.
4. Ne diraj regionalni i centralni server.
5. Nakon otprilike 45-50 sekundi regionalni treba ispisati npr.:

  [REGIONAL][WATCHDOG] CONNECTION_LOST: DRON_001 nije poslao podatke najmanje 45 sekundi.

6. Centralni treba ispisati da je dron oznacen kao CONNECTION_LOST i da je aktivna misija prekinuta.

PROVJERA BAZA
-------------
Regionalna baza:
  sqlite3 REGION_SARAJEVO_regional_server.db
  SELECT drone_uri, status, last_seen FROM drones;
  SELECT drone_uri, alarm_type, message, created_at FROM alarms ORDER BY id DESC;

Centralna baza:
  sqlite3 central_server.db
  SELECT drone_uri, status, last_seen FROM drones;
  SELECT mission_id, drone_uri, status FROM missions;
  SELECT drone_uri, alarm_type, message, created_at FROM alarms ORDER BY id DESC;

Ocekivano nakon timeouta:
  drones.status = CONNECTION_LOST
  missions.status = ABORTED_CONNECTION_LOST   (ako je dron imao ACTIVE misiju)
  alarms.alarm_type = CONNECTION_LOST

NAPOMENA
--------
Ako se isti dron ponovo pokrene i pocne slati status/telemetriju, watchdog ga ponovo smatra aktivnim.
Ako veza kasnije opet nestane, moze se generisati novi CONNECTION_LOST alarm.
