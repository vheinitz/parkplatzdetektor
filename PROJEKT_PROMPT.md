# Projekt-Prompt: Parkassistent (Sensor → Gateway → Server → App)

Dieses Dokument ist die Aufgabenbeschreibung des Gesamtprojekts — gedacht als
Einstieg für Entwickler *und* als Prompt für ein LLM. Es beschreibt Ziel,
Architektur, Schnittstellen und die bereits getroffenen Entscheidungen.

---

## 1. Ziel

Ein Fahrer soll auf dem Handy sehen, welche Stellplätze **in seiner Nähe gerade
frei** sind, einen davon antippen und sofort dorthin navigiert werden.

Nicht Ziel (jedenfalls nicht in Stufe 1): Reservierung, Bezahlung, eigene
Navigation, Nutzerkonten.

## 2. Architektur

```
 Stellplatz            Sammelpunkt              Cloud                   Fahrer
┌──────────┐  Funk    ┌──────────┐  HTTPS   ┌─────────────┐  HTTPS   ┌─────────┐
│  Sensor  │─────────▶│ Gateway  │─────────▶│  API-Server │◀────────▶│   App   │
│ (belegt/ │ LoRa/    │ bündelt  │  POST    │  + DB       │  GET     │         │
│  frei)   │ ESP-NOW/ │ 5–50     │ /events  │             │ /nearby  │         │
└──────────┘ NB-IoT   │ Sensoren │          └─────────────┘          └────┬────┘
                      └──────────┘                                        │
                                                        Deep-Link an die  ▼
                                                        Standard-Navi (Google/Apple Maps)
```

| Komponente | Aufgabe | Stufe 1 (Prototyp) |
|---|---|---|
| Sensor | belegt/frei je Bucht | **entschieden: ESP32-C3 + Magnetometer** — `hardware/carsensor/`, Erkennung funktioniert; Kamera als Alternative für Freiflächen (§7) |
| Gateway | bündelt Sensoren, ein Uplink | **vorgesehen: LoRa**; heute per Skript simuliert (`server/simulate_gateway.py`) |
| **API-Server** | Stammdaten, Status, Umkreissuche | **`server/` in diesem Repo — Flask + SQLite, deployt auf PythonAnywhere** |
| App | Karte, Liste, Navi-Übergabe | Flutter/React Native, später |

**Der Server kennt die Geoposition, der Sensor nicht.** Der Sensor meldet nur
`sensor_id` + `frei/belegt`. Die Koordinaten werden **einmalig beim Einbau** in
der Server-Datenbank hinterlegt. Das hält die Hardware dumm, billig und
austauschbar.

## 3. Datenfluss

1. **Sensor** misst. Gesendet wird **nur bei Zustandswechsel** (plus ein
   Heartbeat alle N Minuten, damit der Server tote Sensoren erkennt). Das spart
   den Großteil von Batterie und Datenvolumen.
2. **Gateway** bündelt und schickt eine Sammelnachricht:
   `{"gateway_id":"GW-1","events":[{"sensor_id":"S-42","status":"free","ts":...}]}`
3. **Server** schlägt `S-42` nach → Koordinaten, Parkplatzname → schreibt Status
   + Zeitstempel, protokolliert das Ereignis für Statistik.
4. **App** fragt `GET /spots/nearby?lat=…&lng=…&radius=500` → sortierte Liste
   der nächsten freien Plätze mit Entfernung in Metern.
5. **App** zeigt die Plätze als Pins/Liste. Der Fahrer tippt einen an, die App
   öffnet die **Standard-Navi-App** des Systems mit genau diesem Ziel.

### 3.1 Navi-Übergabe — entschieden

Die Übergabe eines einzelnen Ziels an die registrierte Navi-App ist der
**Standardweg**, genau so machen es alle vergleichbaren Apps. Keine eigene
Navigation, kein Kartenmaterial, keine Routenberechnung.

```
Android/Universal:  https://www.google.com/maps/dir/?api=1&destination=48.1351,11.5820
iOS (Apple Maps):   http://maps.apple.com/?daddr=48.1351,11.5820
System-Default:     geo:48.1351,11.5820?q=48.1351,11.5820
```

Die API liefert diese drei Links zu jedem Platz gleich mit (`nav`-Feld), damit
die App sie nicht selbst zusammenbauen muss.

Was **nicht** geht: mehrere Ziele gleichzeitig an die Navi übergeben, damit der
Fahrer während der Fahrt durchklickt. Deshalb die Auswahl in der eigenen App —
das ist aber kein Workaround, sondern die übliche Aufteilung.

## 4. API-Vertrag (Stufe 1)

Basis: `/api/v1`. Alles JSON. Authentifizierung per Header `X-API-Key`.
Drei Rollen:

| Rolle | Schlüssel | darf |
|---|---|---|
| App | keiner | `/spots/nearby` lesen |
| Gateway | `PARKING_GATEWAY_KEY` | Status melden |
| Betreiber | `PARKING_ADMIN_KEY` | Stammdaten CRUD, Historie, Statistik |

| Methode | Pfad | Zweck |
|---|---|---|
| `GET` | `/health` | Lebenszeichen |
| `GET` | `/spots/nearby?lat&lng&radius&limit&status&max_age` | **App:** nächste freie Plätze |
| `POST` | `/events` | **Gateway:** Statusmeldungen (einzeln oder Batch) |
| `GET` | `/spots` | Stammdaten auflisten (Filter: `status`, `lot`, `bbox`) |
| `POST` | `/spots` | Platz anlegen |
| `POST` | `/spots/bulk?upsert=1` | viele Plätze auf einmal anlegen |
| `GET` | `/spots/{id}` | ein Platz |
| `PATCH` | `/spots/{id}` | ändern (Position, Name, außer Betrieb) |
| `DELETE` | `/spots/{id}` | löschen |
| `GET` | `/spots/{id}/history` | Ereignisse eines Sensors |
| `GET` | `/stats` | Belegung je Parkplatz |

**Status-Werte:** `free`, `occupied`, `unknown`.
**Veraltung:** meldet ein Sensor länger als `STALE_AFTER_S` (Vorgabe 15 min)
nichts, gilt sein Wert als veraltet und fließt nicht mehr in `/nearby` ein.
Ein „frei“ von gestern ist schlimmer als gar keine Angabe.

## 5. Randbedingungen und Entscheidungen

**Betrieb.** Prototyp auf **PythonAnywhere** (kostenlos, kein Sleep wie bei
Render-Free, Flask + SQLite reichen). Kein Fremdservice nötig — die Datenbank
liegt neben der App. Migration auf Postgres/PostGIS erst, wenn die
Bounding-Box-Suche nicht mehr trägt (grob: > 50 000 Plätze).

**Geosuche.** Bounding-Box im SQL (Index auf `lat`), exakte Haversine-Distanz
und Sortierung in Python. Für Prototyp-Größenordnungen genau und schnell genug.

**Latenz.** Stufe 1: die App pollt (alle 10–30 s bei offener Karte). Push per
MQTT/WebSocket erst, wenn eine echte Flotte dranhängt — für den Prototyp ist
Polling einfacher zu debuggen und zu deployen.

**Fehlauslösungen.** Ein Ultraschallsensor allein hält Fußgänger und Fahrräder
für Autos. Wenn es ernst wird: Doppelsensorik (Ultraschall + Magnetometer im
Boden) oder Kamera je Reihe statt Sensor je Bucht.

**Datenschutz (DSGVO).** Es werden **keine** Kennzeichen, Nutzer-IDs oder
Positionen von Fahrern gespeichert. Das Ereignis-Log enthält nur
„Sensor X war um 14:00 belegt“. Die Umkreisanfrage der App wird nicht
protokolliert. Damit bleibt das System weitgehend außerhalb des Personenbezugs.

**Idempotenz.** Gateway-Meldungen können doppelt oder verspätet ankommen. Der
Server verwirft Meldungen, die älter sind als der gespeicherte Zeitstempel des
Sensors — Retry des Gateways ist damit gefahrlos.

## 6. Meilensteine

1. **Server-API** (fertig, siehe `server/`) — CRUD, Gateway-Endpunkt,
   Umkreissuche, Simulator, Tests.
2. **Deployment** auf PythonAnywhere, Simulator läuft gegen die öffentliche URL.
3. **App** (Flutter): Standort holen → `/nearby` → Liste + Karte → Navi-Deep-Link.
4. **Ein echter Sensor**: ESP32 + HC-SR04, WLAN, POST auf `/events`. ~30 €.
5. **Erst dann** über Skalierung reden: MQTT-Push, Postgres/PostGIS,
   Batterielaufzeit, Gehäuse, dynamische Preise.

## 7. Verbindung zum Kamera-Vorversuch in diesem Repo

Der Vorversuch in `kamera/` (`detect.py`, `fit_grid.py`, `occupancy.py`) löst
dieselbe Aufgabe **ohne Sensor je Bucht**: ein Drohnen-/Mastbild → Raster
fitten → YOLO(DOTA) → belegt/frei je Stellplatz (490 Plätze, ~1,5 s).

Das ist die deutlich billigere Sensorik für Freiflächen. Er passt ohne Bruch in
diese Architektur: die Kamera-Pipeline ist dann **ein Gateway** mit vielen
virtuellen Sensoren — je Rasterzelle eine feste `sensor_id`, ein Bild ergibt
eine Batch-Meldung an `POST /events`.

Offener Punkt dafür: **Georeferenzierung** der Rasterzellen. Vier bekannte
Punkte im Bild (Ecken des Parkplatzes aus einer Karte) genügen für eine
Homographie Pixel → Lat/Lng; damit bekommt jede Zelle einmalig ihre Koordinate
und wird per `POST /spots/bulk` angelegt.

## 8. Offene Fragen

- Freifläche mit Kamera oder Parkhaus mit Sensoren — welcher Pilot zuerst?
- Wer betreibt die Plätze (Kommune, Handel, privat)? Bestimmt, ob Stammdaten
  gepflegt werden können.
- Braucht die App eine Karte, oder reicht Liste + Navi-Übergabe für Stufe 1?
  (Liste ist deutlich billiger — kein Maps-SDK, kein API-Key, keine Kosten.)
- Genauigkeitsanspruch: „ungefähr dort sind Plätze frei“ (Reihen-/Bereichsebene)
  reicht meist und ist viel robuster als „genau Bucht 14“.
