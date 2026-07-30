# Parkassistent — API-Server (Prototyp)

Flask + SQLite, gebaut für den kostenlosen Tarif von **PythonAnywhere**. Drei
Aufgaben:

1. **Betreiber** legt Stellplätze an — `sensor_id` + Geoposition (CRUD).
2. **Gateway** meldet Statuswechsel der Sensoren (`POST /events`).
3. **App** fragt freie Plätze im Umkreis (`GET /spots/nearby`).

Der Sensor kennt seine Position nicht — er meldet nur seine Kennung und
frei/belegt. Die Zuordnung Kennung → Koordinate steht in der Datenbank und wird
einmalig beim Einbau gesetzt. Das hält die Hardware dumm und austauschbar.

Dazu liefert der Server die **Fahrer-App** unter `/app` gleich mit (`app/`) —
eine Web-App ohne Build und ohne Store.

Live: <https://vheinitz.pythonanywhere.com/app>

Gesamtkonzept: [`../PROJEKT_PROMPT.md`](../PROJEKT_PROMPT.md).

## Lokal starten

```bash
pip3 install --user Flask
python3 flask_app.py                       # http://127.0.0.1:5000
python3 seed.py                            # 40 Demo-Plätze anlegen + Startstatus
python3 simulate_gateway.py                # Sensoren simulieren, Strg-C beendet
python3 test_api.py                        # 15 Selbsttests, eigene Test-DB
```

Danach in einem dritten Terminal:

```bash
curl "http://127.0.0.1:5000/api/v1/spots/nearby?lat=48.1351&lng=11.5820&radius=200&limit=3"
```

## Dateien

| Datei | Inhalt |
|---|---|
| `flask_app.py` | die komplette API (der Name ist von PythonAnywhere vorgegeben) |
| `db.py` | Schema und SQLite-Verbindung |
| `geo.py` | Haversine, Bounding-Box, Navi-Deep-Links |
| `config.py` | alle Einstellungen, per Umgebungsvariable überschreibbar |
| `seed.py` | Demo-Stammdaten als Raster anlegen |
| `simulate_gateway.py` | Sensor-Gateway simulieren (Hardware-Ersatz) |
| `test_api.py` | Selbsttests, ohne laufenden Server |
| `wsgi_pythonanywhere.py` | Vorlage für die WSGI-Datei beim Deployment |

## Die Fahrer-App (`/app`)

Eine einzelne HTML-Datei, vom selben Server ausgeliefert — gleiche Herkunft wie
die API, also kein CORS, und die Standortabfrage im Browser bekommt das nötige
HTTPS vom Host geschenkt. Kein Build, kein SDK, kein App-Store, keine
Fremdinhalte (kein CDN, keine Karten-Bibliothek).

- Standort per `watchPosition`, neu geladen erst ab 25 m Bewegung — sonst feuert
  jedes GPS-Zucken eine Anfrage.
- Liste der freien Plätze, nach Entfernung sortiert, mit Alter der Meldung.
- **Antippen → Standard-Navi-App** mit genau diesem einen Ziel (iOS erkennt sie
  und nimmt Apple Maps, sonst Google Maps). Keine eigene Navigation.
- Radius 200 m … 2 km, Auto-Refresh alle 15 s, aber nur wenn die App sichtbar
  ist — im Hintergrund zu pollen kostet nur Akku und Serverlast.
- „Standort setzen" für Geräte ohne GPS und für den Demo-Parkplatz aus `seed.py`.
- Über *Zum Homescreen hinzufügen* läuft sie dank `manifest.json` wie eine
  installierte App im Vollbild.

Auf dem Handy einfach `https://<benutzer>.pythonanywhere.com/app` öffnen.

## API

Basis `/api/v1`, alles JSON, Authentifizierung per Header `X-API-Key`.

| Rolle | Schlüssel | darf |
|---|---|---|
| App | keiner | `/spots/nearby` |
| Gateway | `PARKING_GATEWAY_KEY` | `POST /events` |
| Betreiber | `PARKING_ADMIN_KEY` | alles |

### App: freie Plätze im Umkreis

```
GET /api/v1/spots/nearby?lat=48.1351&lng=11.5820&radius=500&limit=20
```

Parameter: `radius` in Metern (Vorgabe 500, max. 5000), `limit` (Vorgabe 20),
`status` (`free` | `occupied` | `unknown` | `any`), `max_age` in Sekunden
(`0` schaltet die Veraltungsprüfung ab).

```json
{
  "count": 2, "total_in_radius": 16,
  "spots": [{
    "sensor_id": "S-01-002", "name": "Reihe 2, Platz 3", "lot": "Demo-Parkplatz",
    "lat": 48.1351449, "lng": 11.5820673,
    "status": "free", "age_s": 8.1, "stale": false, "distance_m": 1.4,
    "nav": {
      "google": "https://www.google.com/maps/dir/?api=1&destination=48.135145,11.582067",
      "apple":  "http://maps.apple.com/?daddr=48.135145,11.582067",
      "geo":    "geo:48.135145,11.582067?q=48.135145,11.582067"
    }
  }]
}
```

Sortiert nach Entfernung. Die `nav`-Links sind fertig für die Übergabe an die
Standard-Navi-App — die App muss sie nicht selbst bauen und braucht kein
Karten-SDK.

Kein Schlüssel, keine Protokollierung: weder Position noch Zeitpunkt der
Anfrage werden gespeichert.

### Gateway: Status melden

```bash
curl -X POST http://127.0.0.1:5000/api/v1/events \
  -H "Content-Type: application/json" -H "X-API-Key: dev-gateway-key" \
  -d '{"gateway_id":"GW-1","events":[
        {"sensor_id":"S-00-000","status":"free","ts":1785149262,"battery":3.6,"rssi":-92}]}'
```

`ts` darf Unix-Sekunden, Millisekunden oder ISO-8601 sein; fehlt es, gilt die
Empfangszeit. `events` kann entfallen — dann wird der Body als Einzelmeldung
gelesen. Antwort: **immer 200** mit Einzelergebnis je Meldung
(`ok` | `unknown_sensor` | `stale_ignored` | `invalid`). Absicht: ein
unbekannter Sensor darf nicht die ganze Sammelmeldung scheitern lassen, sonst
wiederholt das Gateway ewig.

### Betreiber: Stammdaten

```bash
K='-H "X-API-Key: dev-admin-key"'
curl -X POST .../api/v1/spots      -d '{"sensor_id":"S-42","lat":48.13,"lng":11.58,"name":"Ebene 2 Nr. 14","lot":"P3"}'
curl -X POST .../api/v1/spots/bulk?upsert=1 -d '{"spots":[ ... ]}'
curl       .../api/v1/spots?lot=P3&limit=100
curl -X PATCH  .../api/v1/spots/S-42 -d '{"disabled":true}'
curl -X DELETE .../api/v1/spots/S-42
curl       .../api/v1/spots/S-42/history
curl       .../api/v1/stats
```

`bulk?upsert=1` überschreibt vorhandene Kennungen — Position und Name werden
korrigiert, der laufende Status bleibt erhalten. Das ist der Normalfall beim
Nachmessen.

## Auf PythonAnywhere deployen

1. Kostenlosen Account auf [pythonanywhere.com](https://www.pythonanywhere.com)
   anlegen (Tarif „Beginner").
2. **Files** → Ordner `parkassistent` anlegen, den Inhalt von `server/`
   hochladen. Alternativ in einer **Bash-Konsole**:
   `git clone <repo> parkassistent`
3. **Web** → *Add a new web app* → *Manual configuration* → *Python 3.10+*.
   Nicht „Flask" wählen, sonst wird eine eigene App-Datei angelegt.
4. In der Bash-Konsole:
   ```bash
   pip3 install --user Flask
   ```
5. **Web** → *WSGI configuration file* anklicken, Inhalt komplett durch
   `wsgi_pythonanywhere.py` ersetzen und `<benutzer>` sowie die beiden
   Schlüssel eintragen. Schlüssel erzeugen:
   ```bash
   python3 -c "import secrets; print(secrets.token_urlsafe(32))"
   ```
6. **Reload** klicken. Fertig:
   ```
   https://<benutzer>.pythonanywhere.com/api/v1/health
   ```
7. Von hier aus füllen und testen:
   ```bash
   python3 seed.py --url https://<benutzer>.pythonanywhere.com --key <ADMIN-KEY>
   python3 simulate_gateway.py --url https://<benutzer>.pythonanywhere.com \
           --key <GATEWAY-KEY> --admin-key <ADMIN-KEY>
   ```

### Deploy per API statt per Hand

Ab dem zweiten Mal geht alles über die REST-API — kein Hochladen im Browser,
keine Konsole. Nötig ist ein **API-Token** (Account → *API token*), nicht das
Passwort; ein Token lässt sich einzeln widerrufen.

```bash
echo "<token>" > ~/.pythonanywhere_token && chmod 600 ~/.pythonanywhere_token
export PA_USER=<benutzer>

python3 deploy_pythonanywhere.py --check        # Token, Web-App, CPU-Kontingent
python3 deploy_pythonanywhere.py --dry-run      # zeigt nur, was passieren würde
python3 deploy_pythonanywhere.py --wsgi         # erstes Mal: WSGI-Datei mit erzeugen
python3 deploy_pythonanywhere.py                # danach: hochladen + reload + /health
```

`--wsgi` erzeugt die WSGI-Datei samt zwei zufälligen API-Schlüsseln und legt sie
unter `/var/www/<domain>_wsgi.py` ab. Die Schlüssel landen lokal in
`~/.parkassistent_keys.json` — sie werden beim nächsten Deploy wiederverwendet,
sonst wären alle Gateways ausgesperrt.

Die Datenbank liegt eine Ebene über dem Code-Ordner und wird nie angefasst.

**Ein Schritt bleibt manuell**, einmalig: die Web-App anlegen (Web-Tab →
*Manual configuration* → Python 3.x). Das geht über keine API. Danach läuft
alles über das Skript.

`pip3 install --user Flask` war entgegen der Anleitung oben **nicht nötig** —
Flask ist auf PythonAnywhere vorinstalliert (getestet mit Python 3.13).

### Was auf PythonAnywhere zu beachten ist

- **HTTPS ist inklusive** — der Sensor/das Gateway kann direkt POSTen.
- **Eingehend geht alles**, die bekannte Whitelist des Free-Tarifs betrifft nur
  *ausgehende* Verbindungen des Servers. Dieser Server ruft nichts nach außen —
  also kein Problem. (Betrifft aber `seed.py`/`simulate_gateway.py`, wenn man
  sie *auf* PythonAnywhere gegen einen fremden Host laufen lässt.)
- **Kein Sleep** bei Inaktivität — anders als beim Free-Tarif von Render. Die
  Web-App muss aber alle drei Monate per Klick verlängert werden.
- **Ein Worker.** Für den Prototyp reichlich; SQLite läuft im WAL-Modus, Lesen
  blockiert das Schreiben nicht.
- **Die DB-Datei liegt außerhalb des Code-Ordners** (`/home/<benutzer>/parkassistent/`),
  damit ein `git pull` sie nicht anfasst.

## Entwurfsentscheidungen

**SQLite statt Fremddatenbank.** Die Last ist schreibarm (nur Zustandswechsel)
und leseleicht. Kein zweiter Dienst, kein zweites Konto, kein Netzwerk-Hop.
Wechsel auf Postgres/PostGIS erst, wenn die Bounding-Box-Suche nicht mehr trägt
— grob ab 50 000 Plätzen.

**Umkreissuche zweistufig.** Erst Bounding-Box im SQL (nutzt den Index auf
`lat/lng`), dann exakte Haversine-Distanz in Python auf den paar Kandidaten. Ein
Kreis in Grad-Koordinaten lässt sich nicht indizieren, ein Rechteck schon.

**Veraltung.** Meldet ein Sensor länger als `STALE_AFTER_S` (Vorgabe 15 min)
nichts, fällt er aus `/nearby` heraus. Ein „frei" von gestern ist schlimmer als
gar keine Angabe. Deshalb sendet auch der Simulator einen Heartbeat.

**Idempotenz.** Meldungen mit älterem Zeitstempel als dem gespeicherten werden
verworfen (`stale_ignored`). Ein Retry des Gateways ist damit gefahrlos, auch
wenn Pakete verspätet oder doppelt ankommen.

**Falsche Sensoruhren** werden gedeckelt: ein Zeitstempel weiter als 5 min in
der Zukunft wird auf die Empfangszeit gesetzt. Sonst kippt ein einziger Sensor
mit kaputter Uhr die Veraltungslogik und lässt sich nie wieder überschreiben.

**Zwei getrennte Schlüssel.** Wer die Gateway-Kennung aus einem Sensor
ausliest — und das ist bei Hardware im Feld die Regel — kann damit keine Plätze
löschen oder verschieben.

**DSGVO.** Gespeichert wird nur „Sensor X war um 14:00 belegt". Keine
Kennzeichen, keine Nutzer-IDs, keine Fahrerpositionen. `PARKING_KEEP_EVENTS=0`
schaltet auch das Ereignis-Log ab.

## Einstellungen (Umgebungsvariablen)

| Variable | Vorgabe | Bedeutung |
|---|---|---|
| `PARKING_DB` | `server/parking.sqlite3` | Pfad der Datenbank |
| `PARKING_ADMIN_KEY` | `dev-admin-key` | **im Betrieb ändern** |
| `PARKING_GATEWAY_KEY` | `dev-gateway-key` | **im Betrieb ändern** |
| `PARKING_STALE_AFTER_S` | `900` | ab wann ein Wert als veraltet gilt |
| `PARKING_DEFAULT_RADIUS_M` | `500` | Vorgabe-Suchradius |
| `PARKING_MAX_RADIUS_M` | `5000` | Obergrenze Suchradius |
| `PARKING_DEFAULT_LIMIT` / `MAX_LIMIT` | `20` / `200` | Trefferzahl |
| `PARKING_KEEP_EVENTS` | `1` | Ereignis-Log an/aus |
| `PARKING_MAX_CLOCK_SKEW_S` | `300` | erlaubter Uhrvorlauf der Sensoren |

`/api/v1/health` warnt, solange die Entwicklungsschlüssel aktiv sind.

## Was fehlt (bewusst)

- **Push statt Polling** — MQTT/WebSocket, sobald eine echte Flotte dranhängt.
  Für den Prototyp ist Polling einfacher zu deployen und zu debuggen.
- **Rate-Limiting** der offenen `/nearby`-Route.
- **Reservierung, Bezahlung, Nutzerkonten** — nicht Teil von Stufe 1.
- **Anbindung des Kamera-Detektors** aus diesem Repo als virtuelles Gateway.
  Dafür fehlt die Georeferenzierung der Rasterzellen (siehe
  `../PROJEKT_PROMPT.md`, §7).
