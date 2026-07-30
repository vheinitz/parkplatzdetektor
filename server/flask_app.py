"""Parkassistent -- API-Server (Prototyp).

Drei Aufgaben, drei Nutzergruppen:

  1. Betreiber  legt Stellplaetze an: sensor_id + Geoposition.   (CRUD, Admin-Key)
  2. Gateway    meldet Statuswechsel der Sensoren.               (POST /events, Gateway-Key)
  3. App        fragt freie Plaetze im Umkreis.                  (GET /spots/nearby, offen)

Der Sensor kennt seine Position nicht -- er meldet nur seine Kennung und
frei/belegt. Die Zuordnung Kennung -> Koordinate steht hier in der Datenbank und
wird einmalig beim Einbau gesetzt. Das haelt die Hardware dumm und austauschbar.

Die Datei heisst flask_app.py, weil PythonAnywhere genau diesen Namen erwartet.
Lokal:  python3 flask_app.py   ->  http://127.0.0.1:5000
"""
import hmac
import math
import os
import sqlite3
import time
from datetime import datetime
from functools import wraps

from flask import Flask, jsonify, redirect, request, send_from_directory
from werkzeug.exceptions import HTTPException

import config
import db
from geo import bbox, haversine, nav_links, peilung, winkel_diff

API = '/api/v1'

# Die Fahrer-App liegt als statische Seite daneben und wird vom selben Server
# ausgeliefert: gleiche Herkunft wie die API, also kein CORS, und die
# Standortabfrage im Browser braucht ohnehin HTTPS -- das liefert der Host.
APP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app')


# ---------------------------------------------------------------- Fehler ----

class ApiError(Exception):
    """Fehler mit HTTP-Code -- wird zentral in JSON uebersetzt."""

    def __init__(self, status, message, **extra):
        super().__init__(message)
        self.status = status
        self.message = message
        self.extra = extra


# ------------------------------------------------------------- Parsing ------

def parse_ts(value, received_at):
    """Zeitstempel aus Unix-Sekunden oder ISO-8601. None -> Empfangszeit.

    Sensoruhren gehen falsch. Wer zu weit in der Zukunft liegt, bekommt die
    Empfangszeit -- sonst kippt ein einziger Sensor mit kaputter Uhr die
    Veraltungslogik und laesst sich nie wieder ueberschreiben.
    """
    if value is None:
        return received_at
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        ts = float(value)
        if ts > 1e11:                       # offensichtlich Millisekunden
            ts /= 1000.0
    elif isinstance(value, str):
        try:
            ts = float(value)
            if ts > 1e11:
                ts /= 1000.0
        except ValueError:
            try:
                text = value.strip().replace('Z', '+00:00')
                ts = datetime.fromisoformat(text).timestamp()
            except ValueError:
                raise ApiError(400, 'ts nicht lesbar: %r' % value)
    else:
        raise ApiError(400, 'ts nicht lesbar: %r' % value)
    if not math.isfinite(ts):
        raise ApiError(400, 'ts nicht lesbar: %r' % value)
    return min(ts, received_at + config.MAX_CLOCK_SKEW_S)


def need_float(data, key, lo, hi, default=None):
    """Pflicht-/Optionalfeld als Zahl im gueltigen Bereich."""
    if key not in data or data[key] is None:
        if default is None:
            raise ApiError(400, 'Feld fehlt: %s' % key)
        return default
    try:
        val = float(data[key])
    except (TypeError, ValueError):
        raise ApiError(400, '%s ist keine Zahl: %r' % (key, data[key]))
    if not math.isfinite(val) or not (lo <= val <= hi):
        raise ApiError(400, '%s ausserhalb [%s, %s]: %r' % (key, lo, hi, data[key]))
    return val


def need_status(value):
    if value not in db.STATUSES:
        raise ApiError(400, 'status muss %s sein, nicht %r' % ('/'.join(db.STATUSES), value))
    return value


def json_body():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        raise ApiError(400, 'JSON-Objekt im Body erwartet')
    return data


def clamp_int(raw, default, lo, hi):
    if raw is None or raw == '':
        return default
    try:
        return max(lo, min(hi, int(raw)))
    except ValueError:
        raise ApiError(400, 'ganze Zahl erwartet, nicht %r' % raw)


# ---------------------------------------------------------------- Auth ------

def require_key(role):
    """Zugriffsschutz per X-API-Key. role: 'admin' oder 'gateway'.

    Der Admin-Schluessel darf ueberall, der Gateway-Schluessel nur melden.
    """
    def deco(fn):
        @wraps(fn)
        def wrapper(*a, **kw):
            key = request.headers.get('X-API-Key') or request.args.get('key', '')
            allowed = (config.ADMIN_KEY,) if role == 'admin' else (config.GATEWAY_KEY, config.ADMIN_KEY)
            if key not in allowed:
                raise ApiError(401, 'X-API-Key fehlt oder ist falsch (Rolle: %s)' % role)
            return fn(*a, **kw)
        return wrapper
    return deco


# ----------------------------------------------------------------- App ------

def create_app():
    app = Flask(__name__)
    app.json.sort_keys = False          # Reihenfolge der Felder wie geschrieben
    app.teardown_appcontext(db.close_db)
    db.init_db()

    # Die App laeuft im Browser/WebView unter fremder Herkunft -> CORS offen
    # halten. Geschuetzt wird ueber den API-Key, nicht ueber die Herkunft.
    @app.after_request
    def cors(resp):
        resp.headers['Access-Control-Allow-Origin'] = '*'
        resp.headers['Access-Control-Allow-Headers'] = 'Content-Type, X-API-Key'
        resp.headers['Access-Control-Allow-Methods'] = 'GET, POST, PATCH, PUT, DELETE, OPTIONS'
        return resp

    @app.errorhandler(ApiError)
    def on_api_error(err):
        body = {'error': err.message}
        body.update(err.extra)
        return jsonify(body), err.status

    @app.errorhandler(404)
    def on_404(_e):
        return jsonify({'error': 'unbekannter Endpunkt', 'see': API + '/'}), 404

    @app.errorhandler(HTTPException)
    def on_http_error(err):
        # Alles andere aus Flask/Werkzeug (405, 413, ...) ebenfalls als JSON --
        # ein Gateway soll keine HTML-Fehlerseite parsen muessen.
        return jsonify({'error': err.description}), err.code

    @app.errorhandler(Exception)
    def on_500(err):
        app.logger.exception('unbehandelter Fehler')
        return jsonify({'error': 'interner Fehler', 'detail': str(err)}), 500

    register_routes(app)
    return app


def register_routes(app):

    # -------------------------------------------------- Uebersicht ----------

    @app.get('/')
    @app.get(API + '/')
    def index():
        return jsonify({
            'service': 'parkassistent-api',
            'version': 1,
            'app': '/app',
            'admin': '/admin',
            'endpoints': {
                'GET  ' + API + '/health': 'Lebenszeichen',
                'GET  ' + API + '/spots/nearby?lat&lng&radius&limit': 'App: freie Plaetze im Umkreis',
                'POST ' + API + '/events': 'Gateway: Statusmeldungen (X-API-Key)',
                'GET  ' + API + '/spots': 'Stammdaten auflisten (X-API-Key)',
                'POST ' + API + '/spots': 'Platz anlegen (X-API-Key)',
                'POST ' + API + '/spots/bulk': 'viele Plaetze anlegen (X-API-Key)',
                'GET|PATCH|DELETE ' + API + '/spots/<id>': 'einzelner Platz (X-API-Key)',
                'GET  ' + API + '/spots/<id>/history': 'Ereignisse eines Sensors (X-API-Key)',
                'GET  ' + API + '/stats': 'Belegung je Parkplatz (X-API-Key)',
            },
        })

    # ------------------------------------------------- Fahrer-App -----------

    @app.get('/app')
    def webapp_slash():
        # Ohne Schraegstrich loest der Browser "vendor/leaflet.js" gegen "/" auf
        # und holt /vendor/leaflet.js -> 404. Deshalb umleiten statt ausliefern.
        return redirect('/app/', code=302)

    @app.get('/app/')
    def webapp_index():
        return send_from_directory(APP_DIR, 'index.html')

    @app.get('/app/<path:name>')
    def webapp_file(name):
        # send_from_directory sperrt Pfade ausserhalb von APP_DIR selbst ab.
        return send_from_directory(APP_DIR, name)

    @app.get('/admin')
    def adminseite_slash():
        return redirect('/admin/', code=302)

    @app.get('/admin/')
    def adminseite():
        # Die Seite selbst ist nur eine Huelle ohne Geheimnis. Geschuetzt sind
        # die Daten: sie holt alles ueber die API und muss dafuer den
        # Admin-Schluessel mitschicken, den der Benutzer einmalig eingibt.
        return send_from_directory(APP_DIR, 'admin.html')

    @app.get(API + '/health')
    def health():
        con = db.get_db()
        n = con.execute('SELECT COUNT(*) FROM spots').fetchone()[0]
        warnungen = []
        if config.USING_DEV_KEYS:
            warnungen.append('Standard-Entwicklungsschluessel aktiv')
        if config.USING_DEFAULT_PASSWORD:
            warnungen.append('Standard-Passwort der Verwaltungsseite aktiv')
        return jsonify({
            'ok': True,
            'time': time.time(),
            'spots': n,
            'db': config.DB_PATH,
            'warning': '; '.join(warnungen) or None,
        })

    @app.post(API + '/login')
    def login():
        """Benutzer/Passwort gegen den Admin-Schluessel eintauschen.

        Nur fuer die Verwaltungsseite: dort soll niemand einen 43 Zeichen langen
        Schluessel abtippen. Der Vergleich laeuft ueber compare_digest, damit die
        Antwortzeit nichts ueber das Passwort verraet.
        """
        data = json_body()
        user = data.get('username')
        pw = data.get('password')
        if not isinstance(user, str) or not isinstance(pw, str):
            raise ApiError(400, 'username und password erwartet')
        ok = (hmac.compare_digest(user, config.ADMIN_USER) and
              hmac.compare_digest(pw, config.ADMIN_PASSWORD))
        if not ok:
            raise ApiError(401, 'Benutzer oder Passwort falsch')
        return jsonify({'token': config.ADMIN_KEY, 'role': 'admin', 'user': user})

    # -------------------------------------------------- App: Umkreis --------

    @app.get(API + '/spots/nearby')
    def nearby():
        """Freie Plaetze um einen Punkt, nach Entfernung sortiert.

        Mit "heading" (Fahrtrichtung in Grad) wird stattdessen nach geschaetzter
        **Fahrstrecke** sortiert. Bei Parkluecken am Strassenrand ist die
        Luftlinie naemlich das falsche Mass: eine Luecke 30 m hinter mir kostet
        Wenden und Zurueckfahren, eine 150 m voraus liegt auf dem Weg.

        Bewusst ohne Schluessel: das ist die Abfrage der Fahrer-App. Es werden
        weder Position noch Zeitpunkt der Anfrage protokolliert.
        """
        now = time.time()
        lat = need_float(request.args, 'lat', -90, 90)
        lng = need_float(request.args, 'lng', -180, 180)
        radius = need_float(request.args, 'radius', 1, config.MAX_RADIUS_M, config.DEFAULT_RADIUS_M)
        limit = clamp_int(request.args.get('limit'), config.DEFAULT_LIMIT, 1, config.MAX_LIMIT)
        status = request.args.get('status', 'free')
        # max_age=0 hebt die Veraltungspruefung auf (fuer Debugging)
        max_age = need_float(request.args, 'max_age', 0, 86400 * 30, config.STALE_AFTER_S)

        # Ohne heading bleibt alles wie bisher -- reine Entfernungssuche.
        heading = None
        if request.args.get('heading') not in (None, ''):
            heading = need_float(request.args, 'heading', -360, 720) % 360.0
        # Die Zuschlaege sind pro Anfrage einstellbar, damit sich der Effekt
        # messen laesst, ohne den Server neu zu starten.
        u_turn = need_float(request.args, 'u_turn', 0, 100000, config.U_TURN_M)
        cross = need_float(request.args, 'cross', 0, 100000, config.CROSS_M)

        lat_min, lat_max, lng_min, lng_max = bbox(lat, lng, radius)
        sql = ('SELECT * FROM spots WHERE disabled = 0 '
               'AND lat BETWEEN ? AND ? AND lng BETWEEN ? AND ?')
        args = [lat_min, lat_max, lng_min, lng_max]
        if status != 'any':
            sql += ' AND status = ?'
            args.append(need_status(status))

        found = []
        for row in db.get_db().execute(sql, args):
            dist = haversine(lat, lng, row['lat'], row['lng'])
            if dist > radius:                       # Ecken des Rechtecks wegwerfen
                continue
            spot = db.row_to_spot(row, now)
            if max_age > 0 and (spot['age_s'] is None or spot['age_s'] > max_age):
                continue                            # stumm oder veraltet -> nicht anbieten
            spot['distance_m'] = round(dist, 1)
            spot['nav'] = nav_links(row['lat'], row['lng'])
            if heading is not None:
                spot.update(fahrkosten(lat, lng, row, dist, heading, u_turn, cross))
            found.append(spot)

        schluessel = 'effective_m' if heading is not None else 'distance_m'
        found.sort(key=lambda s: s[schluessel])
        return jsonify({
            'query': {'lat': lat, 'lng': lng, 'radius_m': radius,
                      'status': status, 'max_age_s': max_age, 'limit': limit,
                      'heading': heading, 'sorted_by': schluessel,
                      'u_turn_m': u_turn, 'cross_m': cross},
            'count': min(len(found), limit),
            'total_in_radius': len(found),
            'spots': found[:limit],
        })

    @app.get(API + '/spots/<sensor_id>/status')
    def spot_status(sensor_id):
        """Status eines einzelnen Platzes -- ohne Schluessel, fuer den Ziel-Waechter.

        Die App merkt sich den angesteuerten Platz und fragt hier nach, ob er
        noch frei ist. Gibt bewusst nur Status und Alter zurueck, keine
        Stammdaten -- Position und Name kennt die App ohnehin schon.
        """
        now = time.time()
        row = db.get_db().execute(
            'SELECT status, status_ts, disabled FROM spots WHERE sensor_id = ?',
            (sensor_id,)).fetchone()
        if row is None:
            raise ApiError(404, 'kein Platz mit sensor_id %r' % sensor_id)
        age = None if row['status_ts'] is None else round(now - row['status_ts'], 1)
        return jsonify({
            'sensor_id': sensor_id,
            'status': row['status'],
            'age_s': age,
            'stale': None if age is None else age > config.STALE_AFTER_S,
            'disabled': bool(row['disabled']),
        })

    # -------------------------------------------------- Gateway -------------

    @app.post(API + '/events')
    @require_key('gateway')
    def post_events():
        """Statusmeldungen vom Sensor-Gateway -- einzeln oder als Batch.

        Erwartet:
            {"gateway_id": "GW-1",
             "events": [{"sensor_id": "S-42", "status": "free",
                         "ts": 1721990000, "battery": 3.7, "rssi": -92}, ...]}

        Eine einzelne Meldung ohne "events"-Liste wird auch akzeptiert.

        Antwort ist immer 200 mit Einzelergebnis je Meldung -- ein unbekannter
        Sensor darf nicht die ganze Sammelmeldung scheitern lassen, sonst
        wiederholt das Gateway ewig.
        """
        now = time.time()
        body = json_body()
        gateway_id = body.get('gateway_id')
        events = body.get('events')
        if events is None:
            events = [body]                          # Einzelmeldung
        if not isinstance(events, list):
            raise ApiError(400, '"events" muss eine Liste sein')
        if len(events) > 1000:
            raise ApiError(413, 'hoechstens 1000 Meldungen je Anfrage')

        con = db.get_db()
        results, applied = [], 0
        for i, ev in enumerate(events):
            if not isinstance(ev, dict):
                results.append({'index': i, 'result': 'invalid', 'detail': 'kein Objekt'})
                continue
            sid = ev.get('sensor_id')
            try:
                if not isinstance(sid, str) or not sid.strip():
                    raise ApiError(400, 'sensor_id fehlt')
                sid = sid.strip()
                status = need_status(ev.get('status'))
                ts = parse_ts(ev.get('ts'), now)
            except ApiError as err:
                results.append({'index': i, 'sensor_id': sid, 'result': 'invalid',
                                'detail': err.message})
                continue

            row = con.execute('SELECT status, status_ts FROM spots WHERE sensor_id = ?',
                              (sid,)).fetchone()
            if row is None:
                # Unbekannte Kennung: der Server kennt die Koordinate nicht, also
                # kann er den Platz nicht anlegen. Betreiber muss ihn eintragen.
                results.append({'index': i, 'sensor_id': sid, 'result': 'unknown_sensor'})
                continue
            if row['status_ts'] is not None and ts < row['status_ts']:
                # Verspaetet oder doppelt zugestellt -- Wiederholungen des
                # Gateways sind damit gefahrlos.
                results.append({'index': i, 'sensor_id': sid, 'result': 'stale_ignored'})
                continue

            con.execute('UPDATE spots SET status = ?, status_ts = ?, updated_at = ? '
                        'WHERE sensor_id = ?', (status, ts, now, sid))
            if config.KEEP_EVENTS:
                con.execute('INSERT INTO events (sensor_id, status, ts, received_at, '
                            'gateway_id, battery, rssi) VALUES (?, ?, ?, ?, ?, ?, ?)',
                            (sid, status, ts, now, gateway_id,
                             ev.get('battery'), ev.get('rssi')))
            applied += 1
            results.append({'index': i, 'sensor_id': sid, 'result': 'ok',
                            'status': status, 'changed': status != row['status']})
        con.commit()
        return jsonify({'received': len(events), 'applied': applied, 'results': results})

    # -------------------------------------------------- Stammdaten ----------

    @app.get(API + '/spots')
    @require_key('admin')
    def list_spots():
        now = time.time()
        sql, args = 'SELECT * FROM spots WHERE 1=1', []
        if 'status' in request.args:
            sql += ' AND status = ?'
            args.append(need_status(request.args['status']))
        if 'lot' in request.args:
            sql += ' AND lot = ?'
            args.append(request.args['lot'])
        if 'bbox' in request.args:
            # bbox=lat_min,lng_min,lat_max,lng_max -- fuer den Kartenausschnitt
            try:
                a, b, c, d = (float(x) for x in request.args['bbox'].split(','))
            except ValueError:
                raise ApiError(400, 'bbox=lat_min,lng_min,lat_max,lng_max erwartet')
            sql += ' AND lat BETWEEN ? AND ? AND lng BETWEEN ? AND ?'
            args += [min(a, c), max(a, c), min(b, d), max(b, d)]
        total = db.get_db().execute(
            'SELECT COUNT(*) FROM (' + sql + ')', args).fetchone()[0]

        limit = clamp_int(request.args.get('limit'), 100, 1, 1000)
        offset = clamp_int(request.args.get('offset'), 0, 0, 10 ** 9)
        rows = db.get_db().execute(sql + ' ORDER BY sensor_id LIMIT ? OFFSET ?',
                                   args + [limit, offset])
        return jsonify({'count': total, 'limit': limit, 'offset': offset,
                        'spots': [db.row_to_spot(r, now) for r in rows]})

    @app.get(API + '/spots/<sensor_id>')
    @require_key('admin')
    def get_spot(sensor_id):
        row = db.get_db().execute('SELECT * FROM spots WHERE sensor_id = ?',
                                  (sensor_id,)).fetchone()
        if row is None:
            raise ApiError(404, 'kein Platz mit sensor_id %r' % sensor_id)
        spot = db.row_to_spot(row, time.time())
        spot['nav'] = nav_links(row['lat'], row['lng'])
        return jsonify(spot)

    @app.post(API + '/spots')
    @require_key('admin')
    def create_spot():
        """Platz anlegen. Pflicht: sensor_id, lat, lng."""
        spot = _insert_spot(db.get_db(), json_body(), time.time(), upsert=False)
        db.get_db().commit()
        return jsonify(spot), 201, {'Location': API + '/spots/' + spot['sensor_id']}

    @app.post(API + '/spots/bulk')
    @require_key('admin')
    def create_spots_bulk():
        """Viele Plaetze auf einmal -- der Normalfall beim Einmessen.

        ?upsert=1 ueberschreibt vorhandene Kennungen statt sie abzulehnen.
        """
        now = time.time()
        body = json_body()
        items = body.get('spots')
        if not isinstance(items, list):
            raise ApiError(400, '{"spots": [...]} erwartet')
        if len(items) > 5000:
            raise ApiError(413, 'hoechstens 5000 Plaetze je Anfrage')
        upsert = request.args.get('upsert') in ('1', 'true', 'yes')

        con = db.get_db()
        created, results = 0, []
        for i, item in enumerate(items):
            try:
                if not isinstance(item, dict):
                    raise ApiError(400, 'kein Objekt')
                spot = _insert_spot(con, item, now, upsert=upsert)
                created += 1
                results.append({'index': i, 'sensor_id': spot['sensor_id'], 'result': 'ok'})
            except ApiError as err:
                results.append({'index': i, 'sensor_id': item.get('sensor_id')
                                if isinstance(item, dict) else None,
                                'result': 'error', 'detail': err.message})
        con.commit()
        return jsonify({'received': len(items), 'created': created, 'results': results})

    @app.patch(API + '/spots/<sensor_id>')
    @app.put(API + '/spots/<sensor_id>')
    @require_key('admin')
    def update_spot(sensor_id):
        """Teilaenderung: name, lot, lat, lng, status, disabled."""
        now = time.time()
        data = json_body()
        con = db.get_db()
        row = con.execute('SELECT * FROM spots WHERE sensor_id = ?', (sensor_id,)).fetchone()
        if row is None:
            raise ApiError(404, 'kein Platz mit sensor_id %r' % sensor_id)

        sets, args = [], []

        def put(column, value):
            sets.append(column + ' = ?')
            args.append(value)

        if 'name' in data:
            put('name', _opt_text(data['name'], 'name'))
        if 'lot' in data:
            put('lot', _opt_text(data['lot'], 'lot'))
        if 'lat' in data:
            put('lat', need_float(data, 'lat', -90, 90))
        if 'lng' in data:
            put('lng', need_float(data, 'lng', -180, 180))
        if 'street_bearing' in data:
            put('street_bearing', _opt_bearing(data['street_bearing'], 'street_bearing'))
        if 'disabled' in data:
            put('disabled', 1 if data['disabled'] else 0)
        if 'status' in data:
            # Handeingriff, z.B. Platz gesperrt. Zeitstempel mitziehen, sonst
            # gilt der Wert sofort als veraltet.
            put('status', need_status(data['status']))
            put('status_ts', now)
        if not sets:
            raise ApiError(400, 'keine aenderbaren Felder im Body')

        put('updated_at', now)
        args.append(sensor_id)
        con.execute('UPDATE spots SET ' + ', '.join(sets) + ' WHERE sensor_id = ?', args)
        con.commit()
        row = con.execute('SELECT * FROM spots WHERE sensor_id = ?', (sensor_id,)).fetchone()
        return jsonify(db.row_to_spot(row, now))

    @app.delete(API + '/spots/<sensor_id>')
    @require_key('admin')
    def delete_spot(sensor_id):
        con = db.get_db()
        cur = con.execute('DELETE FROM spots WHERE sensor_id = ?', (sensor_id,))
        con.commit()
        if cur.rowcount == 0:
            raise ApiError(404, 'kein Platz mit sensor_id %r' % sensor_id)
        return '', 204

    @app.get(API + '/spots/<sensor_id>/history')
    @require_key('admin')
    def spot_history(sensor_id):
        """Ereignisse eines Sensors -- fuer Statistik und Fehlersuche."""
        limit = clamp_int(request.args.get('limit'), 100, 1, 1000)
        rows = db.get_db().execute(
            'SELECT status, ts, received_at, gateway_id, battery, rssi FROM events '
            'WHERE sensor_id = ? ORDER BY ts DESC LIMIT ?', (sensor_id, limit))
        return jsonify({'sensor_id': sensor_id, 'events': [dict(r) for r in rows]})

    @app.get(API + '/stats')
    @require_key('admin')
    def stats():
        """Belegung je Parkplatz, plus Zahl der stummen Sensoren."""
        now = time.time()
        rows = db.get_db().execute(
            'SELECT COALESCE(lot, "-") AS lot, status, COUNT(*) AS n, '
            '  SUM(CASE WHEN status_ts IS NULL OR ? - status_ts > ? THEN 1 ELSE 0 END) AS stale '
            'FROM spots WHERE disabled = 0 GROUP BY lot, status',
            (now, config.STALE_AFTER_S))
        out = {}
        for r in rows:
            e = out.setdefault(r['lot'], {'free': 0, 'occupied': 0, 'unknown': 0,
                                          'stale': 0, 'total': 0})
            e[r['status']] = r['n']
            e['stale'] += r['stale']
            e['total'] += r['n']
        return jsonify({'time': now, 'stale_after_s': config.STALE_AFTER_S, 'lots': out})


# ------------------------------------------------------------- Helfer -------

def fahrkosten(lat, lng, row, dist, heading, u_turn, cross):
    """Geschaetzte Fahrstrecke statt Luftlinie, plus die Gruende dafuer.

    Zwei Zuschlaege, beide bewusst grob -- ohne Strassennetz laesst sich mehr
    nicht seriös rechnen:

      hinter mir          -> 2 x Entfernung + Wendekosten (vorbei, wenden, zurueck)
      falsche Strassenseite -> fester Zuschlag (Gegenverkehr queren, umrunden)

    Die zweite Regel greift nur, wenn zum Platz eine Fahrbahnrichtung
    (street_bearing) hinterlegt ist. Ohne die Angabe bleibt das Feld None.
    """
    peil = peilung(lat, lng, row['lat'], row['lng'])
    voraus = abs(winkel_diff(peil, heading)) <= config.AHEAD_HALF_ANGLE
    kosten = dist if voraus else 2 * dist + u_turn

    gleiche_seite = None
    if row['street_bearing'] is not None:
        gleiche_seite = abs(winkel_diff(row['street_bearing'], heading)) <= 90
        if not gleiche_seite:
            kosten += cross

    return {'bearing_deg': round(peil, 1), 'ahead': voraus,
            'same_side': gleiche_seite, 'effective_m': round(kosten, 1)}


def _opt_bearing(value, field):
    if value is None:
        return None
    try:
        val = float(value)
    except (TypeError, ValueError):
        raise ApiError(400, '%s ist keine Zahl: %r' % (field, value))
    if not math.isfinite(val):
        raise ApiError(400, '%s ist keine Zahl: %r' % (field, value))
    return val % 360.0


def _opt_text(value, field):
    if value is None:
        return None
    if not isinstance(value, str):
        raise ApiError(400, '%s muss Text sein' % field)
    return value.strip() or None


def _insert_spot(con, data, now, upsert):
    sid = data.get('sensor_id')
    if not isinstance(sid, str) or not sid.strip():
        raise ApiError(400, 'sensor_id fehlt')
    sid = sid.strip()
    lat = need_float(data, 'lat', -90, 90)
    lng = need_float(data, 'lng', -180, 180)
    name = _opt_text(data.get('name'), 'name')
    lot = _opt_text(data.get('lot'), 'lot')
    bearing = _opt_bearing(data.get('street_bearing'), 'street_bearing')
    status = need_status(data.get('status', 'unknown'))
    status_ts = now if status != 'unknown' else None

    sql = ('INSERT INTO spots (sensor_id, name, lot, lat, lng, street_bearing, '
           'status, status_ts, updated_at, disabled) '
           'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)')
    if upsert:
        # Beim Nachmessen der Position darf der laufende Status nicht verloren
        # gehen -- deshalb werden status/status_ts hier nicht angefasst.
        sql += (' ON CONFLICT(sensor_id) DO UPDATE SET name = excluded.name, '
                'lot = excluded.lot, lat = excluded.lat, lng = excluded.lng, '
                'street_bearing = excluded.street_bearing, '
                'updated_at = excluded.updated_at')
    try:
        con.execute(sql, (sid, name, lot, lat, lng, bearing, status, status_ts, now,
                          1 if data.get('disabled') else 0))
    except sqlite3.IntegrityError:
        raise ApiError(409, 'sensor_id %r existiert bereits '
                            '(?upsert=1 zum Ueberschreiben)' % sid)
    row = con.execute('SELECT * FROM spots WHERE sensor_id = ?', (sid,)).fetchone()
    return db.row_to_spot(row, now)


app = create_app()          # PythonAnywhere/WSGI erwartet ein Modul-Objekt "app"


if __name__ == '__main__':
    if config.USING_DEV_KEYS:
        print('ACHTUNG: Standard-Entwicklungsschluessel aktiv '
              '(dev-admin-key / dev-gateway-key)')
    print('DB: %s' % config.DB_PATH)
    app.run(host='127.0.0.1', port=5000, debug=True)
