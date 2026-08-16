# v9_scheduler - centralna dodjela zadataka po prioritetu

## Ključna ispravka
Prioritet misije više se NE koristi da izbaci dron sa pune rute.

Prioritet se odnosi na RED ČEKANJA ZADATAKA:

MONITORING (4) > DELIVERY (3) > INSPECTION (2) > TEST_FLIGHT (1)

Dron nakon autentifikacije prijavljuje stanje AVAILABLE i čeka START_MISSION.
Centralni server bira slobodan dron i dodjeljuje mu najprioritetniji zadatak koji se trenutno može sigurno izvršiti.

Ruta/visinski slot su zasebna sigurnosna logika. Ako je određena ruta puna, misija ostaje QUEUED; aktivna misija se NE prekida zbog prioriteta.

STOP_MISSION je zadržan kao posebna kontrolna funkcija za eksplicitno zaustavljanje misije.

## Komponente
- central_server: mission queue + scheduler + routing
- regional_server: posrednik i dispatcher START_MISSION/STOP_MISSION komandi
- drone_client: registruje se, postaje AVAILABLE i čeka zadatak
- mission_client: testni/operator CLI za predaju i zaustavljanje zadataka

## Statusi drona
REGISTERED -> AVAILABLE -> BUSY/ON_MISSION -> AVAILABLE
Dodatno: RETURN_TO_BASE, CONNECTION_LOST

## Statusi misije
QUEUED -> ACTIVE -> FINISHED
QUEUED -> CANCELLED
ACTIVE -> STOP_REQUESTED -> STOPPED
ACTIVE -> ABORTED_LOW_BATTERY / ABORTED_CONNECTION_LOST

## Kompajliranje
g++ -std=c++11 -I . central_server.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I . regional_server.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I . drone_client.cpp -o drone_client -lboost_system -pthread
g++ -std=c++11 -I . mission_client.cpp -o mission_client -lboost_system -pthread

## Pokretanje
1) Centralni:
./central_server 9000

2) Regionalni:
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4

3) Dronovi (samo se prijave kao AVAILABLE):
./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120
./drone_client 127.0.0.1 8000 8001 DRON_002 abc123 120

4) Predaja zadataka:
./mission_client 127.0.0.1 8000 M_TEST TEST_FLIGHT SKENDERIJA SKENDERIJA_K1 120
./mission_client 127.0.0.1 8000 M_DEL DELIVERY SKENDERIJA AUTO 120 43.8580 18.4160
./mission_client 127.0.0.1 8000 M_MON MONITORING SKENDERIJA SKENDERIJA_K2 120

Ako nema slobodnih dronova, zadaci ostaju QUEUED. Kad se dron oslobodi, scheduler bira najveći priority.

## Kako dokazati prioritet
Pokreni samo 1 dron i dok je zauzet pošalji redom:
- TEST_FLIGHT
- INSPECTION
- DELIVERY
- MONITORING

Sve nove misije će biti QUEUED. Kad aktivna misija završi, sljedeća dodijeljena treba biti MONITORING, zatim DELIVERY, INSPECTION, TEST_FLIGHT.

SQL:
SELECT mission_id, mission_type, mission_priority, drone_uri, status, created_at
FROM missions
ORDER BY CASE status WHEN 'ACTIVE' THEN 0 WHEN 'QUEUED' THEN 1 ELSE 2 END,
         mission_priority DESC, created_at ASC;

## STOP_MISSION
Aktivna misija:
./mission_client 127.0.0.1 8000 STOP M_MON

Centralni vraća STOP_MISSION_DISPATCH, regionalni šalje STOP_MISSION tačno dodijeljenom dronu, dron vraća ACK_STOP i postaje AVAILABLE. Scheduler zatim može odmah dodijeliti sljedeći QUEUED zadatak.
