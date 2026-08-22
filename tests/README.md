# Testiranje i benchmarking

Testni paket pokriva funkcionalne, sigurnosne i fail-safe scenarije protokola bez izmjene produkcijskog koda. Svaki test pokrece centralni i regionalni server u izolovanom radnom direktoriju unutar `tests/results/`, pa se normalne projektne SQLite baze ne brisu niti mijenjaju.

## Preduslovi

Prvo iz root direktorija projekta:

```bash
./generate_pqc_certs.sh
./build.sh
```

Potrebni alati:

```bash
sudo apt install sqlite3 tcpdump python3 iproute2 coreutils
```

`tcpdump` je potreban samo za UDP replay test. Za taj test potrebne su odgovarajuce capture privilegije (`sudo` ili `cap_net_raw`).

## Svi funkcionalni testovi

Zaustaviti rucno pokrenute servere/dronove na portovima 9000/8000/8001, zatim:

```bash
./tests/test_all.sh
```

Rezime se zapisuje u:

```text
tests/results/test_results.txt
```

Svaki test ima vlastiti direktorij sa logovima centralnog, regionalnog i dronova.

## Pojedinacni testovi

```text
test_auth.sh              URI+TOKEN, INVALID_TOKEN, UNKNOWN_DRONE_URI
test_missions.sh          MONITORING, DELIVERY, INSPECTION, TEST_FLIGHT
test_priority.sh          prioritetni QUEUED scheduler
test_control.sh           PARAMS i STOP_MISSION
test_route_conflict.sh    altitude slotovi, route conflict, PARAMS safety
test_rtb.sh               manual RTB i LOW_BATTERY charging tok
test_formation_failure.sh failure jednog formation clana
test_signal_loss.sh       SIGNAL_LOSS alarm i RTB
test_connection_loss.sh   lokalni failsafe RTB nakon TCP/TLS prekida
test_udp_replay.sh        replay snimljenog AES-GCM UDP datagrama
```

Primjer:

```bash
./tests/test_route_conflict.sh
```

## Benchmark

```bash
ITERATIONS=10 ./tests/benchmark.sh
```

CSV se zapisuje u:

```text
tests/results/benchmark/benchmark_results.csv
```

Mjere se:

- autentifikacija do `AVAILABLE` potvrde,
- `MISSION_SUBMIT` request/response,
- `PARAMS` request/response,
- `STOP` request/response,
- `RTB` request/response.

Vrijednosti su izražene u milisekundama. Skripta na kraju ispisuje prosjek, medijan, p95, minimum i maksimum.

## Konfiguracija

Default test konfiguracija:

```text
CENTRAL_PORT=9000
REGIONAL_TCP_PORT=8000
REGIONAL_UDP_PORT=8001
REGION_ID=REGION_SARAJEVO
```

Po potrebi se mogu promijeniti environment varijablama, npr.:

```bash
CENTRAL_PORT=9100 REGIONAL_TCP_PORT=8100 REGIONAL_UDP_PORT=8101 ./tests/test_auth.sh
```

Testni tokeni se nalaze u `tests/test_credentials.conf` i namijenjeni su iskljucivo automatizovanom testiranju.
