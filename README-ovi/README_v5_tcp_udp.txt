AUTONOMNI DRONOVI - v5 TCP + UDP
================================

Promjena u odnosu na v4_priority:
- TCP ostaje za registraciju, autentifikaciju, misije, alarme, RTB i ACK poruke.
- UDP se koristi za TELEMETRY i KEEPALIVE od drona prema regionalnom serveru.
- Centralni server ostaje TCP i ne zahtijeva promjenu protokola prema regionalnom serveru.

PORTOVI U PRIMJERU
------------------
Centralni server TCP:          9000
Regionalni server TCP za dron: 8000
Regionalni server UDP za dron: 8001

KOMPajLIRANJE
-------------
Ako je include folder u istom direktoriju:

g++ -std=c++11 -I include dostava_central_server_v5_tcp_udp.cpp -o central_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I include dostava_regional_server_v5_tcp_udp.cpp -o regional_server -lboost_system -lsqlite3 -pthread
g++ -std=c++11 -I include dostava_drone_client_v5_tcp_udp.cpp -o drone_client -lboost_system -pthread

POKRETANJE
----------
1) Centralni server:
./central_server 9000

2) Regionalni server:
./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4,BASCARSIJA:43.8590:18.4310:800:4

3) Monitoring dron:
./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120

4) Delivery dron:
./drone_client 127.0.0.1 8000 8001 DRON_DEL abc123 SKENDERIJA DELIVERY AUTO 120 43.8580 18.4160

WIRESHARK FILTERI
-----------------
tcp.port == 8000
udp.port == 8001

Ocekivano:
- na TCP 8000: REGISTER_REQ, AUTH_REQ, MISSION_REQUEST, ALARM, MISSION_FINISHED, ACK_RTB...
- na UDP 8001: TELEMETRY svake 3 s i KEEPALIVE svakih 15 s.

NAPOMENA
--------
UDP poruke nemaju ACK. Ako se jedan TELEMETRY datagram izgubi, ne retransmituje se stari podatak; sljedeci periodicki podatak ga zamjenjuje.
