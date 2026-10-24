# Parkassistent

Prototyp, der freie Parklücken anzeigt: ein Magnetsensor je Lücke meldet
„belegt“ oder „frei“, ein Server sammelt die Meldungen, eine Web-App zeigt die
nächsten freien Lücken und übergibt die gewählte an die Navi-App.
Schulprojekt für Jugend forscht.

Ein Auto verzerrt das Erdmagnetfeld messbar; ein Magnetometer unter dem
Fahrzeug erkennt daran die Belegung, ohne Kamera. Die Sensoren sollen per LoRa
an ein Gateway melden, das die Meldungen an den Server weitergibt. LoRa wurde
gewählt, weil ein Sensor am Bordstein kein WLAN hat und nur bei
Zustandswechsel sendet; ein Stromsparbetrieb (Schlafmodus) ist im
Sensor-Sketch noch nicht umgesetzt.

```
Magnetsensor je Lücke  --LoRa-->  Gateway  --HTTPS-->  Server  <--HTTPS--  App
 ESP32-C3 + GY-271                                    Flask + SQLite       Web-App
   hardware/carsensor             gateway/              server/         server/app
```

## Aufbau des Repos

| Pfad | Inhalt |
|---|---|
| [`server/`](server/README.md) | Hauptteil: API-Server, Fahrer-App (`/app`) und Verwaltungsseite (`/admin`). Flask + SQLite, für PythonAnywhere eingerichtet. |
| [`gateway/`](gateway/README.md) | LoRa-Empfänger (ESP32-C3 + SX1278, Sketch) und `gateway.py`, das dessen Meldungen über die serielle Schnittstelle liest und an den Server schickt. |
| [`hardware/carsensor/`](hardware/carsensor/carsensor.ino) | Stellplatzsensor: ESP32-C3 + GY-271 (QMC5883L/QMC5883P/HMC5883L), Kalibrierung und Feldversuch über ein Captive Portal, optional LoRa (SX1278, 433,5 MHz). |
| [`hardware/espnow_cam/`](hardware/espnow_cam/README.md) | Bildübertragung ESP32-CAM → ESP-NOW → Browser, Hilfswerkzeug für Aufnahmen vom Parkplatz. |
| [`kamera/`](kamera/README.md) | Vorversuch: freie Plätze aus einem Drohnenbild zählen (Raster fitten + YOLO). |
| [`PROJEKT_PROMPT.md`](PROJEKT_PROMPT.md) | Ziel, Architektur, API-Vertrag, Randbedingungen, offene Fragen. |
| `docs/` | Linksammlung der Recherche, Lernpfad (HTML), Datenblatt QMC5883L. |

## Stand

| Teil | Zustand |
|---|---|
| Server, API, Datenbank | läuft, 24 Tests (`server/test_api.py`) |
| Fahrer-App (Karte, Umkreissuche, Ziel-Wächter) | läuft |
| Verwaltungsseite | läuft |
| Magnetsensor: Erkennung belegt/frei | am Tisch erprobt, nicht im Feld |
| `gateway.py` | 23 Tests (`gateway/test_gateway.py`), mit `--replay` gegen den Server durchgespielt |
| LoRa-Sender und -Empfänger (Sketches) | kompilieren für den ESP32-C3, Funkstrecke noch nicht erprobt |
| Kamera-Vorversuch | abgeschlossen; Video, Einzelbilder und YOLO-Gewichte sind nicht im Repo |

Bis zum ersten Funkversuch mit den SX1278-Modulen ersetzt
`server/simulate_gateway.py` das Gateway; beide nutzen denselben
`POST /api/v1/events`.

Eine Instanz läuft unter <https://vheinitz.pythonanywhere.com> (App unter
`/app`, API unter `/api/v1`).

## Schnellstart

```bash
cd server
pip3 install --user Flask
python3 flask_app.py                  # Server auf http://127.0.0.1:5000
# in einem zweiten Terminal:
python3 seed.py                       # 40 Demo-Plätze anlegen
python3 simulate_gateway.py           # Sensoren simulieren, Strg-C beendet
python3 test_api.py                   # Tests, eigene Test-Datenbank
```

Dann <http://127.0.0.1:5000/app> öffnen. Deployment, API und Einstellungen
stehen in [`server/README.md`](server/README.md).

## Lizenz

[MIT](LICENSE)
