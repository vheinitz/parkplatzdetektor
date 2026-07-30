# Gateway — von der Parklücke zum Server

Der fehlende Weg zwischen Sensor und Server. Ein Parksensor am Bordstein hat
kein WLAN und keine Steckdose; er funkt per LoRa. Dieses Projekt nimmt den Funk
an und bringt ihn ins Netz.

```
Parksensor          Empfänger              Rechner / Raspberry        Server
┌──────────┐ LoRa  ┌──────────────┐ USB   ┌──────────────┐  HTTPS   ┌────────┐
│ ESP32-C3 │──────▶│ ESP32 + RFM95│──────▶│  gateway.py  │─────────▶│  API   │
│ + RFM95  │ 868   │ lora_gateway │ 115200│ prüft, bündelt│ /events  │        │
└──────────┘ MHz   └──────────────┘  Baud └──────────────┘          └────────┘
```

| Datei | Aufgabe |
|---|---|
| `firmware/lora_gateway/lora_gateway.ino` | Empfänger. Nimmt Pakete an, prüft die Prüfsumme, schreibt eine Zeile je Paket auf USB. Sonst nichts. |
| `gateway.py` | Läuft auf dem Rechner. Liest Zeilen, wirft Wiederholungen weg, setzt den Zeitstempel, bündelt und schickt an `POST /api/v1/events`. |
| `test_gateway.py` | 23 Tests der Rechnerseite — ohne Funkmodul, ohne Server. |

Der Sender sitzt im Sensor selbst:
[`../hardware/carsensor/carsensor.ino`](../hardware/carsensor/carsensor.ino),
Abschnitt „LoRa-Sender".

## Warum die Arbeit so verteilt ist

Der Mikrocontroller im Empfänger **versteht das Protokoll nicht**. Er prüft die
Prüfsumme und reicht die Zeile weiter. Das ist Absicht: ein Gateway, das den
Inhalt auswertet, müsste bei jeder Protokolländerung neu geflasht werden — und
es steht womöglich auf einem Mast. Eines, das nur weiterreicht, nie.

Der **Zeitstempel entsteht auf dem Rechner**, nicht im Sensor. Eine Uhr je
Parklücke wäre Bauteil, Strom und Kalibrieraufwand für nichts; die Laufzeit
über Funk liegt bei Millisekunden.

Die **Warteschlange liegt auf dem Rechner**. Fällt das Internet aus, sammeln
sich Meldungen an — dafür hat der Rechner Speicher, der Mikrocontroller nicht.
Je Sensor wird nur die letzte Meldung aufgehoben: ein überholter Zwischenstand
interessiert niemanden mehr.

## Teile

| Teil | Anzahl | ungefähr |
|---|---|---|
| SX1276/RFM95W-Modul, 868 MHz | 1 je Sensor + 1 fürs Gateway | 5–8 € |
| ESP32-C3 (Sensor hat schon einen) | 1 fürs Gateway | 4 € |
| Antenne: 8,6 cm Draht (λ/4 bei 868 MHz) | je Modul | — |

Alternativ ein Board mit fest verbautem Funkmodul (Heltec WiFi LoRa 32, TTGO
LoRa32) — dann entfällt das Löten, die Pins stehen im Sketch als Kommentar.

**868 MHz für Europa.** 433 MHz ist hier ein Amateurfunkband; 915 MHz gilt in
den USA. Sender und Empfänger müssen dasselbe Band, dieselbe Bandbreite und
dasselbe `LORA_SYNCWORD` haben, sonst hören sie sich nicht.

## Verdrahtung

Beide Seiten gleich. Am Sensor liegen GPIO 4 und 5 schon am Magnetometer,
und GPIO 2, 8, 9 bleiben frei — das sind Strapping-Pins, ein falscher Pegel
beim Einschalten verhindert den Start.

| RFM95 | ESP32-C3 |
|---|---|
| VCC | **3V3** — das Modul ist nicht 5-V-fest |
| GND | GND |
| SCK | GPIO 6 |
| MISO | GPIO 1 |
| MOSI | GPIO 7 |
| NSS | GPIO 10 |
| RST | GPIO 3 |
| DIO0 | nicht nötig |
| ANT | 8,6 cm Draht |

DIO0 bleibt frei, weil beide Sketches das Interruptregister pollen statt einen
Interrupt zu verdrahten. Das spart eine Leitung und einen der knappen freien
Pins. **Nie ohne Antenne senden** — das kann die Endstufe zerstören.

## Protokoll

Über die Luft, als Text (28 Zeichen), damit ein Mitschnitt lesbar bleibt:

```
PS1,<knoten>,<F|B|?>,<mV>,<seq>,<crc>
PS1,PS-A1B2C3,F,3712,17,5057
```

| Feld | Bedeutung |
|---|---|
| `PS1` | Protokollversion. Fremde Pakete fallen sofort durch. |
| `knoten` | Kennung des Sensors, z. B. `PS-A1B2C3` |
| `F` `B` `?` | frei, belegt, **weiß nicht** (nicht kalibriert oder Sensorfehler) |
| `mV` | Batteriespannung, `-1` wenn kein Spannungsteiler bestückt |
| `seq` | laufende Nummer je Ereignis — Wiederholungen tragen dieselbe |
| `crc` | CRC-16/CCITT über alles davor |

Auf USB kommt dieselbe Zeile mit Empfangsdaten:

```
RX,PS1,PS-A1B2C3,F,3712,17,5057,-97,9.5
                                 ↑    ↑
                              RSSI  SNR
```

Zeilen mit `#` sind Kommentar (Startmeldung, Lebenszeichen, verworfene
Pakete). Befehle zum Empfänger: `PING` (antwortet `PONG…`, damit `gateway.py`
den richtigen Port findet), `POLL` (Ringpuffer noch einmal ausgeben, falls der
Rechner später dazukam), `STAT` (Zähler).

Die **Prüfsumme wird zweimal geprüft** — im Empfänger und noch einmal in
`gateway.py`. Der zweite Durchgang deckt die USB-Strecke ab: ein auf dem Kabel
verschlucktes Byte fiele sonst nirgends auf. Dieselbe Funktion steht in beiden
Sketches und in Python; ein Test hält die drei Fassungen zusammen
(`123456789` → `0x29B1`).

### Warum es keine Bestätigung gibt

Das Gateway bestätigt nichts. Der Sensor müsste danach horchen, das kostet
Wachzeit und eine zweite Funkrichtung. Stattdessen geht jedes Ereignis
**dreimal** raus, mit zufälligem Abstand von ~1,5 s. Alle drei tragen dieselbe
laufende Nummer, und `gateway.py` wirft die Doppelten weg. Das ist die übliche
Bauform für batteriebetriebene Einwegknoten.

Der zufällige Abstand ist kein Schmuck: fährt ein Auto an mehreren Lücken
vorbei, senden mehrere Sensoren fast gleichzeitig los. Bei festen Abständen
würden sich auch die Wiederholungen wieder überlagern.

### Sendezeit

Auf 868 MHz darf in Europa 1 % der Zeit gesendet werden — etwa 36 s je Stunde.
Ein Paket dieser Länge braucht bei SF7/BW125 rund 60 ms, drei Wiederholungen
also 0,18 s. Selbst 100 Ereignisse in einer Stunde blieben mit 18 s darunter.
Eine echte Parklücke wechselt ein paar Mal am Tag.

Deshalb sendet der Sensor **nur bei Wechsel**, plus alle 15 min ein
Lebenszeichen. Das Lebenszeichen ist nötig, weil der Server sonst einen stummen
Sensor nicht von einem dauerhaft belegten Platz unterscheiden könnte — er
verwirft Meldungen, die älter als `PARKING_STALE_AFTER_S` (15 min) sind.

## Benutzen

**1. Empfänger flashen**

```bash
arduino-cli lib install LoRa
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc firmware/lora_gateway
arduino-cli upload  -b esp32:esp32:esp32c3:CDCOnBoot=cdc -p /dev/ttyACM0 firmware/lora_gateway
```

**2. Sensor flashen.** In `carsensor.ino` steht `USE_LORA 1`. Jeder Sensor
leitet seine Kennung aus der eigenen MAC-Adresse ab (`PS-A1B2C3`) — 50 Geräte
brauchen also **nicht** 50 verschiedene Quelltexte. Die Kennung steht beim
Start auf der Konsole und unter `i`.

**3. Platz anlegen**, sonst kennt der Server die Kennung nicht:

```bash
curl -X POST https://…/api/v1/spots -H 'X-API-Key: ADMIN' \
     -d '{"sensor_id":"PS-A1B2C3","lat":50.0776,"lng":8.2365,"name":"Lücke 1"}'
```

Oder bequemer auf der Verwaltungsseite unter `/admin`.

**4. Gateway starten**

```bash
pip3 install --user pyserial
python3 gateway.py --port auto --url https://user.pythonanywhere.com --key GATEWAY-KEY
```

| Option | |
|---|---|
| `--port auto` | fragt jeden seriellen Port mit `PING` — findet den richtigen selbst |
| `--dry-run` | zeigt nur, was gesendet würde |
| `--replay DATEI` | liest Zeilen aus einer Datei statt vom Port, ganz ohne Hardware |
| `--interval 5` | Sekunden, über die gebündelt wird |
| `--map DATEI.json` | Zuordnung Knoten → `sensor_id`, `{"PS-A1B2C3": "RH-R03"}` |
| `--verbose` | zeigt auch die verworfenen Wiederholungen |

Auf einem Raspberry als Dauerläufer: `systemd`-Unit mit `Restart=always`. Das
Skript überlebt USB-Abrisse und Serverausfälle selbst, aber nicht den Strom.

## Reichweite messen

Der Feldversuch, der sich für Jugend forscht auswerten lässt:

1. Gateway an den Rechner, `python3 gateway.py --port auto --dry-run --verbose`
2. Sensor mitnehmen, an der seriellen Konsole `f` drücken — das erzwingt eine
   Meldung, ohne auf ein Auto zu warten
3. Am Gateway RSSI und SNR ablesen und mit dem Ort notieren

RSSI ist die Empfangsstärke in dBm (−120 ist sehr schwach, −40 sehr stark),
SNR der Abstand zum Rauschen in dB. LoRa liest noch unter dem Rauschen mit,
SNR darf also negativ sein. Wird es zu knapp, hilft `LORA_SF 9` oder `12`:
mehr Reichweite, dafür längere Sendezeit — und die zählt aufs 1-%-Budget.

## Android statt Rechner

Der Empfänger lässt sich per **USB-OTG** auch an ein Handy hängen; dann
ersetzt eine App den Rechner. Für einen Stand ist das hübsch — kein Laptop,
kein Netzteil.

Gebaut ist das **nicht**. Was es bräuchte:

- Android USB Host API plus einen CDC-Treiber; die übliche Wahl ist die
  Bibliothek `usb-serial-for-android`. Ein OTG-Adapter genügt an Hardware.
- Dieselbe Zeilenverarbeitung wie in `gateway.py` — die ist knapp 100 Zeilen
  und in Kotlin genauso kurz. `test_gateway.py` beschreibt das erwartete
  Verhalten Fall für Fall und taugt als Vorlage.
- Denselben `POST /api/v1/events` mit dem Gateway-Schlüssel im Header.
- Einen Vordergrunddienst mit Benachrichtigung, sonst schläft Android die App
  nach wenigen Minuten ein.

Der Aufwand steckt nicht im Protokoll, sondern im Android-Drumherum
(Berechtigungen, Dienst, Akkuoptimierung). Für den Anfang ist der Raspberry
der kürzere Weg; die App lohnt, wenn das Gateway mobil sein soll.

## Getestet — und was nicht

**Auf dem Rechner geprüft:**

- 23 Tests für `gateway.py`: Prüfsumme, Zerlegen kaputter Zeilen, Wiederholung
  erkennen, Neustart des Sensors (Nummer fängt wieder bei 1 an), unkalibrierte
  Sensoren aussortieren, Warteschlange bei Serverausfall, Bündelung.
- Die CRC-Funktion aus **beiden Sketches** mit `gcc` übersetzt und gegen die
  Python-Fassung gerechnet — vier Testfälle, identisch.
- Ganze Kette gegen einen laufenden Server: Mitschnitt mit echten Prüfsummen →
  `gateway.py` → `POST /events`. Wiederholungen fielen weg, ein unbekannter
  Sensor wurde benannt, `belegt` und `frei` landeten mit Batteriespannung und
  RSSI in der Ereignisliste.

**Nicht geprüft, weil hier kein Funkmodul und kein arduino-cli vorhanden ist:**

- Die beiden Sketches sind **nicht übersetzt** worden. Rechne mit Tippfehlern
  beim ersten `arduino-cli compile`.
- Kein Byte ging je über Funk. Pinbelegung, Sendeleistung und Reichweite sind
  aus den Datenblättern, nicht gemessen.
- Die Sendezeit von ~60 ms ist gerechnet, nicht gestoppt.

Der erste Versuch mit echter Hardware wird also Nacharbeit brauchen. Der
Rechnerteil dagegen läuft und lässt sich mit `--replay` jederzeit ohne Funk
vorführen.
