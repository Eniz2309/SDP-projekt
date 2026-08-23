# Check-lista praktičnog dijela projekta

Ova check-lista prikazuje ispunjenost zahtjeva definisanih za praktični dio
projekta iz predmeta **Softverski dizajn protokola**.

## Dizajn i dokumentacija

- [x] Dizajniran protokol u skladu sa zahtjevima projektnog zadatka
- [x] Izrađen SDD dokument
- [x] Dokumentovani svi osnovni elementi i scenariji rada protokola

## Implementacija protokola

- [x] Protokol implementiran u C++
- [x] Implementirana vlastita simulacija komponenti protokola
- [x] Implementiran centralni server
- [x] Implementiran regionalni server
- [x] Implementirani klijenti autonomnih dronova
- [x] Implementirana aplikacija za kreiranje i slanje misija
- [x] Protokol nije realizovan kao peer-to-peer signalizacioni protokol
- [x] Komunikacija dronova odvija se putem serverske infrastrukture
- [x] Protokol koristi TCP/IP stek

## Asinhrona i sinhrona komunikacija

- [x] Implementirana sinhrona komunikacija
- [x] Implementirana asinhrona komunikacija
- [x] U dokumentaciji je objašnjeno kada i zašto se koristi sinhrona komunikacija
- [x] U dokumentaciji je objašnjeno kada i zašto se koristi asinhrona komunikacija

## Multicast i broadcast

- [x] Protokol ne koristi broadcast komunikaciju
- [x] Multicast komunikacija nije korištena

> Multicast je opcionalan zahtjev i nije korišten u implementiranom protokolu.

## Byte-stream i data-stream komunikacija

- [x] Implementirana byte-stream razmjena podataka
- [x] Implementirana data-stream razmjena podataka
- [x] Korišten TCP transportni protokol
- [x] Korišten UDP transportni protokol
- [x] U dokumentaciji je objašnjena namjena TCP komunikacije
- [x] U dokumentaciji je objašnjena namjena UDP komunikacije
- [x] Dokumentovane su poruke protokola i način njihovog prijenosa

## Sigurnost komunikacije

- [x] Implementirana TLS zaštita komunikacije
- [x] Korišten OpenSSL
- [x] Implementirana/podržana post-kvantna kriptografija (PQC)
- [x] Implementirana TLS + PQC komunikacija
- [x] Implementirana razmjena i provjera sigurnosnih certifikata

## Testiranje na udaljenim mašinama

- [x] Protokol testiran na najmanje dvije udaljene mašine
- [x] Testiranje izvršeno korištenjem virtuelnih mašina
- [x] Centralni i regionalni server testirani na VM1
- [x] Dronovi testirani na VM2
- [x] Provjerena komunikacija između udaljenih komponenti sistema

Korišteno testno okruženje:

```text
VM1: 100.100.129.87
- Central Server
- Regional Server

VM2: 100.100.129.115
- Drone clients
```

## PCAP / Wireshark

- [x] Snimljen saobraćaj finalne verzije protokola
- [x] Dostavljena `.pcap` datoteka
- [x] `.pcap` datoteka sadrži saobraćaj za sve use-case scenarije

## Benchmarking i testne skripte

- [x] Implementirane testne skripte
- [x] Testne skripte pokrivaju zahtjeve definisane u projektnoj dokumentaciji
- [x] Testirani normalni scenariji rada
- [x] Testirani neispravni i vanredni scenariji
- [x] Testne skripte opisane i objašnjene u dokumentaciji

## Mrežni protokol

- [x] Protokol nije baziran na HTTP protokolu
- [x] Nije razvijan aplikacijski API
- [x] Razvijen je vlastiti mrežni protokol

## Git repozitorij

- [x] Izvorni kod dostupan putem Git repozitorija
- [x] U repozitoriju je navedeno da je projekat realizovan na Univerzitetu u Sarajevu
- [x] Naveden Elektrotehnički fakultet
- [x] Naveden Odsjek za telekomunikacije
- [x] Navedeni autori projekta
- [x] Naveden predmet Softverski dizajn protokola

## MSC dijagrami

- [x] Izrađeni MSC dijagrami
- [x] MSC dijagrami pokrivaju sve relevantne use-case scenarije
- [x] Svaki MSC scenario je opisan u dokumentaciji
- [x] Uz scenarije su prikazani odgovarajući dijagrami

## Izvorni i izvršni kod

- [x] Dostavljen kompletan izvorni kod
- [x] Dostavljene skripte za kompajliranje projekta
- [x] Dostavljene binarno kompajlirane izvršne datoteke

## Uputstvo za korištenje

- [x] Dostavljeno uputstvo za instalaciju potrebnih zavisnosti
- [x] Dostavljeno uputstvo za kompajliranje projekta
- [x] Dostavljeno uputstvo za pokretanje centralnog servera
- [x] Dostavljeno uputstvo za pokretanje regionalnog servera
- [x] Dostavljeno uputstvo za pokretanje dronova
- [x] Dostavljeno uputstvo za pokretanje mission client aplikacije
- [x] Opisana konfiguracija certifikata za komunikaciju između udaljenih mašina

## Završna check-lista

- [x] SDD dokument
- [x] Implementacija mrežnog protokola
- [x] TCP komunikacija
- [x] UDP komunikacija
- [x] Sinhrona komunikacija
- [x] Asinhrona komunikacija
- [x] Byte-stream komunikacija
- [x] Data-stream komunikacija
- [x] TLS
- [x] PQC
- [x] Testiranje na dvije udaljene mašine
- [x] Testne skripte
- [x] MSC dijagrami za use-case scenarije
- [x] Izvorni kod
- [x] Uputstvo za instalaciju i pokretanje
- [x] Finalni PCAP za sve use-case scenarije
- [x] Binarno kompajlirane izvršne datoteke
