# Instalacija i pokretanje projekta

Ovo uputstvo opisuje pripremu Ubuntu 22.04 sistema, instalaciju svih potrebnih
zavisnosti, instalaciju OpenSSL 3.5, kompajliranje projekta i pokretanje
centralnog servera, regionalnog servera i klijenata dronova.

---

## 1. Ažuriranje sistema

```bash
sudo apt update
sudo apt upgrade -y
```

---

## 2. Instalacija osnovnih paketa

Instalirati alate potrebne za kompajliranje C++ aplikacija i rad projekta:

```bash
sudo apt install -y \
    build-essential \
    g++ \
    gcc \
    make \
    git \
    wget \
    curl \
    perl \
    pkg-config \
    ca-certificates \
    sqlite3 \
    libsqlite3-dev \
    nlohmann-json3-dev \
    zlib1g-dev
```

Opcionalno, za analizu mrežnog saobraćaja:

```bash
sudo apt install -y tcpdump wireshark
```

---

# 3. Instalacija OpenSSL 3.5

Ubuntu 22.04 standardno koristi stariju verziju OpenSSL-a, dok projekt koristi
OpenSSL 3.5.

Provjera trenutne verzije:

```bash
openssl version
```

OpenSSL 3.5 instalira se odvojeno u:

```text
/usr/local/openssl
```

Na ovaj način se ne mijenja sistemska OpenSSL instalacija.

---

## 3.1 Preuzimanje OpenSSL-a

```bash
cd /tmp

wget https://www.openssl.org/source/openssl-3.5.0.tar.gz
tar -xzf openssl-3.5.0.tar.gz

cd openssl-3.5.0
```

---

## 3.2 Konfiguracija

```bash
./Configure \
    --prefix=/usr/local/openssl \
    --openssldir=/usr/local/openssl \
    shared \
    zlib
```

---

## 3.3 Kompajliranje

```bash
make -j$(nproc)
```

Instalacija:

```bash
sudo make install
```

---

## 3.4 Provjera instalacije

OpenSSL se sada nalazi na:

```bash
/usr/local/openssl/bin/openssl
```

Potrebno je podesiti putanju do njegovih biblioteka:

```bash
export PATH=/usr/local/openssl/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/openssl/lib64:$LD_LIBRARY_PATH
```

Provjera:

```bash
/usr/local/openssl/bin/openssl version -a
```

Očekivana verzija:

```text
OpenSSL 3.5.0
```

---

# 4. Trajno podešavanje OpenSSL okruženja

Da se varijable ne bi morale postavljati nakon svakog prijavljivanja, dodati ih
u `~/.bashrc`:

```bash
echo 'export PATH=/usr/local/openssl/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/openssl/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
```

Zatim:

```bash
source ~/.bashrc
```

Ponovo provjeriti:

```bash
which openssl
openssl version
```

Očekivano:

```text
/usr/local/openssl/bin/openssl
```

i:

```text
OpenSSL 3.5.0
```

---

# 5. Provjera SQLite instalacije

```bash
sqlite3 --version
```

Provjera development biblioteke:

```bash
pkg-config --modversion sqlite3
```

---

# 6. Provjera C++ kompajlera

```bash
g++ --version
```

Projekt zahtijeva kompajler sa podrškom za moderni C++ standard.

---

# 7. Preuzimanje projekta

```bash
cd ~

git clone https://github.com/Eniz2309/SDP-projekt.git
cd SDP-projekt
```

---

# 8. Dozvole za skripte

Skriptama je potrebno omogućiti izvršavanje:

```bash
chmod +x build.sh
```

Ako postoje dodatne skripte:

```bash
chmod +x *.sh
```

---

# 9. Kompajliranje projekta

Prije kompajliranja provjeriti da su postavljene OpenSSL putanje:

```bash
export PATH=/usr/local/openssl/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/openssl/lib64:$LD_LIBRARY_PATH
```

Zatim:

```bash
./build.sh
```

Nakon uspješnog kompajliranja trebaju biti generisane izvršne aplikacije:

```text
central_server
regional_server
drone_client
mission_client
```

Provjera:

```bash
ls -lh central_server regional_server drone_client mission_client
```

---

# 10. Važna napomena za OpenSSL

Kod kompajliranja je potrebno koristiti zaglavlja i biblioteke iste OpenSSL
instalacije.

Za OpenSSL instaliran u `/usr/local/openssl` koriste se:

```text
Headers:
    /usr/local/openssl/include

Libraries:
    /usr/local/openssl/lib64
```

Ako se koriste zaglavlja sistemskog OpenSSL-a, a aplikacija se poveže sa
bibliotekama OpenSSL 3.5, mogu se pojaviti greške zbog nekompatibilnih verzija.

Primjer potrebnih opcija kompajlera:

```bash
-I/usr/local/openssl/include
-L/usr/local/openssl/lib64
-Wl,-rpath,/usr/local/openssl/lib64
```

Biblioteke:

```bash
-lssl -lcrypto
```

Za SQLite:

```bash
-lsqlite3
```

Za višedretveni rad:

```bash
-pthread
```

---

# 11. Certifikati

Za TLS komunikaciju potrebno je generisati odgovarajuće certifikate.

Ako projekt sadrži skriptu za generisanje certifikata:

```bash
chmod +x generate_pqc_certs.sh
./generate_pqc_certs.sh
```

Nakon generisanja provjeriti sadržaj direktorija sa certifikatima:

```bash
ls -l certs/
```

---

# 12. Komunikacija između različitih mašina

Ako se serverske i klijentske komponente pokreću na različitim računarima ili
virtuelnim mašinama, potrebno je razmijeniti potrebne certifikate između njih.

Serverska i klijentska strana moraju imati certifikate potrebne za provjeru
identiteta druge strane.

Bez odgovarajuće razmjene certifikata TLS autentifikacija neće biti uspješna.

---

# 13. Firewall

Ako je firewall uključen:

```bash
sudo ufw status
```

potrebno je omogućiti portove koje koristi projekt.

Primjer:

```bash
sudo ufw allow <TCP_PORT>/tcp
sudo ufw allow <UDP_PORT>/udp
```

Portovi trebaju odgovarati vrijednostima definisanim u konfiguraciji projekta.

Za laboratorijsko testiranje firewall se može privremeno isključiti:

```bash
sudo ufw disable
```

Ovo se ne preporučuje na produkcijskim sistemima.

---

# 14. Provjera mrežne komunikacije

Prije pokretanja projekta provjeriti komunikaciju između mašina.

Primjer:

```bash
ping <IP_ADRESA_DRUGE_MASINE>

# 15. Pokretanje na jednoj mašini

Komponente se pokreću u različitim terminalima.

## Terminal 1 – Centralni server

```bash
./central_server
```

## Terminal 2 – Regionalni server

```bash
./regional_server
```

## Terminal 3 – Dron

```bash
./drone_client
```

Za simulaciju više dronova moguće je pokrenuti više instanci:

```bash
./drone_client
```

u dodatnim terminalima.

## Terminal – Slanje misije

```bash
./mission_client
```

Tačni argumenti zavise od IP adresa, portova i parametara definisanih u
implementaciji.

---



# 18. Baze podataka

Projekt koristi SQLite3.

Baze se mogu pregledati pomoću:

```bash
sqlite3 <ime_baze>.db
```

Unutar SQLite konzole preporučeno je uključiti pregledniji prikaz:

```sql
.headers on
.mode column
```

Pregled tabela:

```sql
.tables
```

Primjer pregleda dronova:

```sql
SELECT * FROM drones;
```

Izlazak:

```sql
.quit
```

---

# 19. Snimanje mrežnog saobraćaja

Za snimanje komunikacije svih use-case scenarija može se koristiti `tcpdump`.

Na serverskoj mašini:

```bash
sudo tcpdump -i any -s 0 -w SDP_FINAL_ALL_USECASES.pcap not port 22
```

Zatim pokrenuti sve testne scenarije.

Snimanje se prekida kombinacijom:

```text
Ctrl+C
```

Dobijeni fajl:

```text
SDP_FINAL_ALL_USECASES.pcap
```

može se otvoriti u Wiresharku.


# 22. Česta greška – pogrešna OpenSSL biblioteka

Ako se pojavi greška slična:

```text
version `OPENSSL_3.x.x' not found
```

potrebno je ponovo postaviti:

```bash
export LD_LIBRARY_PATH=/usr/local/openssl/lib64:$LD_LIBRARY_PATH
```

i provjeriti:

```bash
/usr/local/openssl/bin/openssl version
```

Također:

```bash
ldd /usr/local/openssl/bin/openssl
```

treba pokazivati biblioteke iz:

```text
/usr/local/openssl/lib64
```bash
./build.sh
```

Ako se projekt uspješno kompajlira, instalacija potrebnih zavisnosti je
završena.
