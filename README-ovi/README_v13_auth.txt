AUTONOMNI DRONOVI - v13_auth
============================

CILJ
====
v13 popravlja autentifikaciju iz v12. Regionalni server vise NE prihvata svaki
neprazan token. URI+TOKEN se provjeravaju na CENTRALNOM serveru.

TOK
===
1. Dron uspostavi PQC TLS sesiju sa regionalnim serverom.
2. Dron salje REGISTER_REQ {DRONE_URI, TOKEN, ...}.
3. Regionalni salje DRONE_REGISTER centralnom serveru.
4. Centralni provjerava URI i SHA-256 hash tokena u tabeli drone_credentials.
5. Ako je par validan, centralni veze dron za region i vraca DRONE_REGISTER_OK.
6. Regionalni vraca REGISTER_ACK dronu.
7. Dron salje AUTH_REQ.
8. Regionalni salje DRONE_AUTH centralnom.
9. Centralni ponovo provjerava URI, token, enabled i registered_region.
10. Tek nakon DRONE_AUTH_OK dron dobija AUTH_ACK, postaje AVAILABLE i pocinje
    slati TELEMETRY/KEEPALIVE.

CREDENTIAL REGISTAR
===================
Centralni po defaultu cita:

  drone_credentials.conf

Format svake linije:

  DRONE_URI TOKEN

Primjer:

  DRON_001 abc123
  DRON_002 drugiToken456

Drugi fajl se moze zadati:

  export SDP_DRONE_CREDENTIALS_FILE=/putanja/moji_dronovi.conf

Centralni NE cuva raw token u SQLite bazi. U drone_credentials se cuva SHA-256
hash tokena.

VAZNO: demo .conf sadrzi jednostavne testne tokene radi demonstracije. Za javni
repo je bolje commitovati samo primjer fajla (bez pravih tajni).

BUILD
=====
  chmod +x build_v13_auth.sh
  ./build_v13_auth.sh

POKRETANJE
==========
1) Centralni:

  ./central_server 9000

2) Regionalni:

  ./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 \
      43.8563 18.4131 SKENDERIJA:43.8563:18.4131:1000:4

3) Ispravan dron:

  ./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120

Ocekivano na centralnom:

  [CENTRAL][AUTH] REGISTER OK: DRON_001 @ REGION_SARAJEVO
  [CENTRAL][AUTH] AUTH OK: DRON_001 @ REGION_SARAJEVO

Ocekivano na dronu:

  REGISTER_ACK
  AUTH_ACK
  Authenticated. Starting timers and waiting for assignment.

NEGATIVNI TESTOVI
=================
A) Pogresan token:

  ./drone_client 127.0.0.1 8000 8001 DRON_001 POGRESAN_TOKEN 120

Ocekivano:

  [DRONE][AUTH] Registration rejected: INVALID_TOKEN

Dron NE smije postati AVAILABLE i NE smije pokrenuti telemetry/keepalive timere.

B) Nepoznat URI:

  ./drone_client 127.0.0.1 8000 8001 DRON_HACKER abc123 120

Ocekivano:

  [DRONE][AUTH] Registration rejected: UNKNOWN_DRONE_URI

C) Provjera baze:

  sqlite3 central_server.db

  SELECT drone_uri, enabled, registered_region, created_at, last_auth_at
  FROM drone_credentials;

Raw TOKEN se ne nalazi u tabeli.

RESET REGION BINDINGA ZA TEST
=============================
Prvi uspjesni REGISTER veze credential za region. Ako zelis isti URI kasnije
namjerno testirati na drugom regionalnom serveru:

  sqlite3 central_server.db \
    "UPDATE drone_credentials SET registered_region='' WHERE drone_uri='DRON_001';"

SIGURNOSNE OSOBINE v13
======================
- credential registry je na centralnom serveru;
- nepoznat URI se odbija;
- pogresan token se odbija;
- disabled credential se odbija;
- isti URI se ne moze autentifikovati preko drugog regiona nakon bindinga;
- TLS sesija je vezana za URI koji je uspjesno prosao REGISTER;
- token se u centralnoj bazi cuva kao SHA-256 hash, ne kao otvoren tekst;
- sav REGISTER/AUTH saobracaj i dalje prolazi kroz PQC TLS 1.3 kanal.
