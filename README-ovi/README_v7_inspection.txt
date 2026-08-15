AUTONOMNI DRONOVI - v7_inspection
=================================

Nova funkcionalnost u odnosu na v6_watchdog:
- INSPECTION misija sa 4 inspection tacke na dodijeljenoj konturi.
- Centralni server generise I1, I2, I3 i I4 na uglovima odabrane kvadratne konture.
- Dron obilazi tacke redom I1 -> I2 -> I3 -> I4.
- Nakon dolaska na svaku tacku dron TCP-om salje INSPECTION_REPORT.
- Regionalni server prosljedjuje INSPECTION_REPORT centralnom serveru.
- Centralni server cuva izvjestaje u tabeli inspection_reports.
- INSPECTION misija se moze zavrsiti tek kada centralni ima sva 4 izvjestaja sa RESULT=OK.

Nova protokolska poruka:
------------------------
DRON -> REGIONALNI -> CENTRALNI (TCP)

{
  "TYPE": "INSPECTION_REPORT",
  "MISSION_ID": "DRON_INS_M001",
  "DRONE_URI": "DRON_INS",
  "POINT_ID": "I1",
  "LAT": 43.8585,
  "LON": 18.4100,
  "RESULT": "OK"
}

Odgovor:
{
  "TYPE": "ACK_INSPECTION_REPORT",
  "MISSION_ID": "DRON_INS_M001",
  "POINT_ID": "I1"
}

INSPECTION_POINTS
-----------------
Kod MISSION_APPROVED za INSPECTION centralni server dodaje:

"INSPECTION_POINTS": [
  {"POINT_ID":"I1", "LAT":..., "LON":...},
  {"POINT_ID":"I2", "LAT":..., "LON":...},
  {"POINT_ID":"I3", "LAT":..., "LON":...},
  {"POINT_ID":"I4", "LAT":..., "LON":...}
]

Tacke su uglovi iste konture/rute koja je dodijeljena misiji.

PRIORITET
---------
INSPECTION zadrzava prioritet 2:
MONITORING = 4
DELIVERY   = 3
INSPECTION = 2
TEST_FLIGHT = 1

KOMPAJLIRANJE
-------------
g++ -std=c++11 -I . dostava_central_server_v7_inspection.cpp -o central_server -lboost_system -lsqlite3 -pthread

g++ -std=c++11 -I . dostava_regional_server_v7_inspection.cpp -o regional_server -lboost_system -lsqlite3 -pthread

g++ -std=c++11 -I . dostava_drone_client_v7_inspection.cpp -o drone_client -lboost_system -pthread

POKRETANJE
----------
1) Centralni:
./central_server 9000

2) Regionalni:
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4

3) INSPECTION dron:
./drone_client 127.0.0.1 8000 8001 DRON_INS abc123 SKENDERIJA INSPECTION SKENDERIJA_K2 120

Moze i AUTO ruta:
./drone_client 127.0.0.1 8000 8001 DRON_INS abc123 SKENDERIJA INSPECTION AUTO 120

OCEKIVANI TOK
--------------
[DRONE] INSPECTION mission started. Points=4
[DRONE] Flying to inspection point I1...
[DRONE] Inspection I1 completed. RESULT=OK
[DRONE] Flying to inspection point I2...
[DRONE] Inspection I2 completed. RESULT=OK
[DRONE] Flying to inspection point I3...
[DRONE] Inspection I3 completed. RESULT=OK
[DRONE] Flying to inspection point I4...
[DRONE] Inspection I4 completed. RESULT=OK
[DRONE] All inspection points completed.
[DRONE] Mission finished: DRON_INS_M001

PROVJERA BAZE
-------------
sqlite3 central_server.db

SELECT mission_id, drone_uri, mission_type, route_id, status
FROM missions
WHERE mission_type='INSPECTION';

SELECT mission_id, drone_uri, point_id, lat, lon, result, received_at
FROM inspection_reports
ORDER BY id;

Ocekivanje: 4 reda za misiju (I1, I2, I3, I4), svi RESULT=OK, a misija FINISHED.

NAPOMENA
--------
- TELEMETRY i KEEPALIVE i dalje idu UDP-om.
- INSPECTION_REPORT ide TCP-om jer je to izvjestaj koji ne smije biti izgubljen.
- Watchdog iz v6 ostaje aktivan (45 s timeout).
- DELIVERY, MONITORING, TEST_FLIGHT, prioriteti i RTB ostaju nepromijenjeni.
