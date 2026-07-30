# Parkassistent

Freie Parklücken am Straßenrand finden, ohne im Kreis zu fahren.

Die Idee: **jede Parklücke bekommt einen Magnetsensor.** Ein Auto besteht aus
so viel Eisen, dass es das Erdmagnetfeld messbar verzerrt — daran erkennt ein
billiger Sensor unter dem Fahrzeug zuverlässig „belegt" oder „frei", ganz ohne
Kamera und ohne Bild. Die Sensoren melden **über LoRa** an ein Gateway, das
Gateway bündelt die Meldungen und schickt sie an den **Server**. Die App im
Auto fragt dort nach den nächsten freien Lücken und übergibt die gewählte ans
Navi.

```
Magnetsensor je Lücke  --LoRa-->  Gateway  --HTTPS-->  Server  <--HTTPS--  App
 ESP32-C3 + GY-271                bündelt            Flask + SQLite       PWA
```

Warum LoRa: ein Sensor am Bordstein hat keinen Strom und kein WLAN. LoRa
kommt mit Batterie und Kilometern Reichweite aus, und weil nur bei
Zustandswechsel gesendet wird, bleibt die Datenmenge winzig.

Schulprojekt für Jugend forscht.

## Aufbau des Repos

| Pfad | Inhalt |
|---|---|
| [`server/`](server/README.md) | **Hauptteil.** API-Server, Fahrer-App und Verwaltungsseite. Flask + SQLite, läuft auf PythonAnywhere. |
| [`hardware/carsensor/`](hardware/carsensor/carsensor.ino) | Der Stellplatzsensor: ESP32-C3 + GY-271-Magnetometer, mit Kalibrierung und Feldversuch über ein Captive Portal. |
| [`hardware/espnow_cam/`](hardware/espnow_cam/README.md) | Bildstrecke ESP32-CAM → ESP-NOW → Browser. Entstanden als Werkzeug, um überhaupt ein Bild vom Parkplatz zu bekommen. |
| [`kamera/`](kamera/README.md) | Vorversuch: freie Plätze aus **einem** Luftbild zählen (Raster fitten + YOLO). Zeigt die Alternative ohne Sensor je Bucht. |
| [`PROJEKT_PROMPT.md`](PROJEKT_PROMPT.md) | Ziel, Architektur, API-Vertrag, Randbedingungen, offene Fragen. |
| `docs/` | Recherchenotizen. |

## Stand

Läuft: <https://vheinitz.pythonanywhere.com> — App unter `/app`,
Verwaltung unter `/admin`, API unter `/api/v1`. In der Datenbank stehen
146 Testsensoren (Rheinstraße Wiesbaden, Mikroforumring Wendelsheim,
ein Demo-Parkplatz).

| Teil | Zustand |
|---|---|
| Server, API, Datenbank | fertig, 24 Tests |
| Fahrer-App (Karte, Umkreissuche, Ziel-Wächter) | fertig |
| Verwaltungsseite | fertig |
| Magnetsensor: Erkennung belegt/frei | funktioniert am Tisch |
| **LoRa-Funkstrecke Sensor → Gateway** | **noch offen** — der Sensor meldet bisher nur an sein eigenes Captive Portal |
| Gateway | bisher nur simuliert (`server/simulate_gateway.py`) |
| Kamera-Vorversuch | abgeschlossen, 490 Plätze in ~1,5 s |

Der Weg vom Sensor zum Server ist damit die nächste offene Strecke: heute
füttert der Simulator den Server, morgen soll es das echte LoRa-Gateway sein.
Der Server merkt den Unterschied nicht — für ihn ist beides derselbe
`POST /api/v1/events`.

## Schnellstart

```bash
cd server
python3 flask_app.py                  # lokal auf :5000
python3 seed.py                       # Demo-Plätze anlegen
python3 simulate_gateway.py           # Sensoren simulieren
python3 -m pytest test_api.py -q      # Tests
```

Dann <http://127.0.0.1:5000/app> öffnen. Einzelheiten, Deployment und der
API-Vertrag stehen in [`server/README.md`](server/README.md).
