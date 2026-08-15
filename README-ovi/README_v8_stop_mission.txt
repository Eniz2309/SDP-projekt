SDP PROJEKT - v8_stop_mission
=============================

NOVO U v8
---------
V8 zavrsava aktivni STOP_MISSION tok kod preemptiona misije nizim prioritetom.

Do sada je centralni server mogao oznaciti staru misiju kao
PREEMPTED_BY_HIGHER_PRIORITY i dodijeliti njen slot novoj misiji, ali regionalni
server nije imao mapu aktivnih TCP konekcija dronova i zato nije mogao poslati
STOP_MISSION bas preemptovanom dronu.

U v8 regionalni server pamti:

    DRONE_URI -> aktivna TCP konekcija

Nakon preemptiona tok je:

    novi dron -> MISSION_REQUEST -> regionalni -> centralni
                                              |
                                              | odabir stare misije
                                              v
                                  PREEMPTED_BY_HIGHER_PRIORITY
                                              |
                                              v
    stari dron <- STOP_MISSION <- regionalni
         |
         | prekida lokalnu misiju
         v
      ACK_STOP -> regionalni -> centralni
                                  |
                                  v
                       PREEMPTED_STOP_CONFIRMED

STOP_MISSION poruka
-------------------
Primjer:

{
  "TYPE": "STOP_MISSION",
  "DRONE_URI": "DRON_TEST_1",
  "MISSION_ID": "DRON_TEST_1_M001",
  "REASON": "PREEMPTED_BY_HIGHER_PRIORITY",
  "REPLACED_BY_MISSION_ID": "DRON_MON_M001"
}

ACK_STOP poruka
---------------
Dron nakon prekida misije salje:

{
  "TYPE": "ACK_STOP",
  "DRONE_URI": "DRON_TEST_1",
  "MISSION_ID": "DRON_TEST_1_M001",
  "REASON": "PREEMPTED_BY_HIGHER_PRIORITY",
  "STATUS": "IDLE",
  "BATTERY": 100,
  "LAT": 43.8563,
  "LON": 18.4131,
  "ALTITUDE": 120
}

STA DRON PREKIDA
----------------
Kada primi STOP_MISSION, dron:
- postavlja status na IDLE
- brise active_route_id
- prekida kretanje po konturi
- resetuje waypoint
- prekida INSPECTION stanje
- prekida DELIVERY stanje
- sprjecava naknadno slanje MISSION_FINISHED stare misije
- salje ACK_STOP

REGIONALNI SERVER
-----------------
Regionalni server sada ima mapu aktivnih konekcija po DRONE_URI.
Socket se registruje kada stigne REGISTER_REQ i uklanja se kada TCP konekcija
prestane da postoji.

Upisi na isti TCP socket su zasticeni mutexom da odgovor sesije i asinhrona
STOP_MISSION komanda ne bi bili upisani istovremeno.

CENTRALNI SERVER
----------------
Kada primi ACK_STOP, centralni mijenja status stare misije iz:

    PREEMPTED_BY_HIGHER_PRIORITY

u:

    PREEMPTED_STOP_CONFIRMED

i status starog drona postavlja na IDLE.

Ako regionalni nema aktivnu TCP konekciju preemptovanog drona, centralnom se
salje alarm:

    STOP_MISSION_DELIVERY_FAILED

KOMPajLIRANJE
-------------
Pretpostavlja se ista struktura projekta kao u prethodnim verzijama, odnosno:
- json/json.h
- sqlite3_wrapper.h

Centralni:

g++ -std=c++11 -I . dostava_central_server_v8_stop_mission.cpp \
    -o central_server -lboost_system -lsqlite3 -pthread

Regionalni:

g++ -std=c++11 -I . dostava_regional_server_v8_stop_mission.cpp \
    -o regional_server -lboost_system -lsqlite3 -pthread

Dron:

g++ -std=c++11 -I . dostava_drone_client_v8_stop_mission.cpp \
    -o drone_client -lboost_system -pthread

POKRETANJE
----------
1. Centralni:

./central_server 9000

2. Regionalni:

./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 \
43.8563 18.4131 \
SKENDERIJA:43.8563:18.4131:1000:4

TEST PREEMPTIONA
----------------
Kapacitet jedne konture je 3 drona.

Brzo pokrenuti tri TEST_FLIGHT drona na istoj ruti:

./drone_client 127.0.0.1 8000 8001 DRON_TEST_1 abc123 SKENDERIJA TEST_FLIGHT SKENDERIJA_K1 120
./drone_client 127.0.0.1 8000 8001 DRON_TEST_2 abc123 SKENDERIJA TEST_FLIGHT SKENDERIJA_K1 120
./drone_client 127.0.0.1 8000 8001 DRON_TEST_3 abc123 SKENDERIJA TEST_FLIGHT SKENDERIJA_K1 120

Dok su njihove misije jos ACTIVE, pokrenuti MONITORING dron na istoj ruti:

./drone_client 127.0.0.1 8000 8001 DRON_MON abc123 SKENDERIJA MONITORING SKENDERIJA_K1 120

Prioriteti:
- MONITORING = 4
- DELIVERY = 3
- INSPECTION = 2
- TEST_FLIGHT = 1

Ocekivano:
- centralni bira jednu aktivnu TEST_FLIGHT misiju
- stara misija -> PREEMPTED_BY_HIGHER_PRIORITY
- novi MONITORING dobija njen altitude slot
- regionalni salje STOP_MISSION starom dronu
- stari dron ispisuje STOP_MISSION primljen
- stari dron salje ACK_STOP
- centralni ispisuje da je ACK_STOP potvrdjen
- stara misija -> PREEMPTED_STOP_CONFIRMED

SQL PROVJERA
------------
Na centralnoj bazi:

SELECT mission_id, drone_uri, mission_type, mission_priority,
       route_id, altitude_slot, status
FROM missions
ORDER BY created_at;

Za zaustavljenu misiju treba se vidjeti:

    PREEMPTED_STOP_CONFIRMED

OSTALO
-----
Sve funkcionalnosti iz v7 ostaju:
- TCP kontrolne poruke
- UDP TELEMETRY i KEEPALIVE
- watchdog / CONNECTION_LOST
- TEST_FLIGHT
- MONITORING
- DELIVERY
- INSPECTION + INSPECTION_REPORT
- LOW_BATTERY / RETURN_TO_BASE
- prioriteti misija
- SQLite evidencija
