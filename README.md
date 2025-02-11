# SDP-projekt
Repozitorij za projekt iz predmeta Softverski Dizajn Protokola - Razvoj protokola za javni gradski prijevoz


Dizajnirati i implementirati protokol za naplatu korištanja usluga javnog prevoza. Protokol se sastoji iz dva
dijela: server za obradu zahtjeva/podataka i korisničkih uređaja. Server treba da podrži proizvoljan broj
korisničkih uređaja. Protokol treba da omogući sljedeće funkcionalnosti:
• korisničkim uređajima se smatraju naplatne stanice koje koriste korisnici za plaćanje usluga javnog prevoza
(npr. naplatne stanice u tramvaju, autobusu ili trolejbusu).
• registraciju korisničkih uređaja na server uz pomoć URI-a (jedinstvenog alfanumeričkog identifikatora),
• vođenje registra aktivnih korisničkih uređaja i registrovanih korisnika na serveru:
• korisnik ima mogućnost rezervacije sjedišta u dostupnom vozilu javnog prevoza.
• pored individialne karte za prevoz, korisnici mogu koristiti u i grupne karte zasnovane na različitim
modalitetima (poslovni paket, porodični paket, turistički paket i dr).
– nepohodno je implementirati servere za svaku od vrste prevoznog saobraćaja (tramvajski, trolejbuski
i autobusni) te centralni server koji će sumirati sve prikupljene informacije svih grupa i svih korisnika.
• vođenje registra cijena usluga na centralnom serveru:
– ažuriranje cjenovnika usluga,
– ažuriranje dostupnih prevoznih vozila i usluga,
– ažuriranje dustupnih kapaciteta svakog od vozila.
• korisnici prije korištanja usluge mogu registrovati proizvoljan broj grupa s tim da jedan učesnik može
pripadati više grupa
– za svaku grupu je neophodno odabrati voditelja grupa
– voditelj grupe ima pravo odstraniti članove grupe po volji
– učesnik može zahtjevati brisanje korisničkog naloga koje mora odobriti administrator
• potrebno je uspostaviti sistem naplate za svaku od dostupnih vrsta javnog prevoza (npr. vremenska
individualna karta za tramvajski saobraćaj = 1KM, dužinska indidivdualna karta za trolejbuski saobraćaj
(2 stanice) = 0.5KM i sl. )
• uspostaviti sistem popust usluga zasnovanog na posebnim terminima. Npr. popusti od 30% za usluge prevoza u vrijeme zagušenja cestovnog saobraćaja. popusti od 10% za odabranu grupu korisnika (penzioneri
ili studenti).
• uspostavisti regionalni sistem koji obuhvata najmanje dva odvojena servera te implementirati komunikaciju
server-server.
