"""Selbsttest der API -- ohne laufenden Server, gegen eine eigene Test-DB.

    python3 test_api.py            (oder: python3 -m unittest test_api -v)

Nur Standardbibliothek plus Flask; kein pytest noetig.
"""
import os
import tempfile
import time
import unittest

TMP_DB = os.path.join(tempfile.mkdtemp(prefix='parking-test-'), 'test.sqlite3')
os.environ['PARKING_DB'] = TMP_DB           # muss vor dem Import von config stehen

import config                               # noqa: E402
import db                                   # noqa: E402
import flask_app                            # noqa: E402
from geo import haversine                   # noqa: E402

ADMIN = {'X-API-Key': config.ADMIN_KEY}
GW = {'X-API-Key': config.GATEWAY_KEY}
MUC = (48.1351, 11.5820)


class ApiTest(unittest.TestCase):

    def setUp(self):
        con = db.connect(TMP_DB)
        con.executescript('DELETE FROM spots; DELETE FROM events;')
        con.commit()
        con.close()
        self.app = flask_app.create_app()
        self.c = self.app.test_client()

    # -- Hilfen -------------------------------------------------------------

    def add(self, sid, lat=MUC[0], lng=MUC[1], **kw):
        body = {'sensor_id': sid, 'lat': lat, 'lng': lng}
        body.update(kw)
        return self.c.post('/api/v1/spots', json=body, headers=ADMIN)

    def report(self, sid, status, ts=None):
        ev = {'sensor_id': sid, 'status': status}
        if ts is not None:
            ev['ts'] = ts
        return self.c.post('/api/v1/events', headers=GW,
                           json={'gateway_id': 'GW-TEST', 'events': [ev]})

    # -- Stammdaten ---------------------------------------------------------

    def test_crud(self):
        r = self.add('S-1', name='Platz 1', lot='P3')
        self.assertEqual(r.status_code, 201)
        self.assertEqual(r.json['status'], 'unknown')     # noch nichts gemeldet

        self.assertEqual(self.add('S-1').status_code, 409)  # doppelte Kennung

        r = self.c.get('/api/v1/spots/S-1', headers=ADMIN)
        self.assertEqual(r.json['name'], 'Platz 1')
        self.assertIn('google.com/maps', r.json['nav']['google'])

        r = self.c.patch('/api/v1/spots/S-1', json={'name': 'Platz 1a', 'disabled': True},
                         headers=ADMIN)
        self.assertEqual(r.json['name'], 'Platz 1a')
        self.assertTrue(r.json['disabled'])

        self.assertEqual(self.c.delete('/api/v1/spots/S-1', headers=ADMIN).status_code, 204)
        self.assertEqual(self.c.get('/api/v1/spots/S-1', headers=ADMIN).status_code, 404)

    def test_auth(self):
        self.assertEqual(self.c.get('/api/v1/spots').status_code, 401)
        self.assertEqual(self.c.post('/api/v1/events', json={}).status_code, 401)
        # Gateway-Key darf melden, aber keine Stammdaten sehen
        self.assertEqual(self.c.get('/api/v1/spots', headers=GW).status_code, 401)
        self.assertEqual(self.c.get('/api/v1/spots/nearby?lat=48&lng=11').status_code, 200)

    def test_validierung(self):
        self.assertEqual(self.c.post('/api/v1/spots', json={'sensor_id': 'X'},
                                     headers=ADMIN).status_code, 400)
        self.assertEqual(self.add('X', lat=95).status_code, 400)
        self.assertEqual(self.c.get('/api/v1/spots/nearby?lat=48').status_code, 400)
        self.assertEqual(self.c.get('/api/v1/spots/nearby?lat=abc&lng=11').status_code, 400)

    def test_bulk_upsert(self):
        body = {'spots': [{'sensor_id': 'B-%d' % i, 'lat': MUC[0], 'lng': MUC[1]}
                          for i in range(5)]}
        r = self.c.post('/api/v1/spots/bulk', json=body, headers=ADMIN)
        self.assertEqual(r.json['created'], 5)

        # ohne upsert: alle abgelehnt, aber die Anfrage selbst bleibt 200
        r = self.c.post('/api/v1/spots/bulk', json=body, headers=ADMIN)
        self.assertEqual(r.json['created'], 0)

        # upsert behaelt den gemeldeten Status -- nur die Position wird korrigiert
        self.report('B-0', 'free')
        body['spots'][0]['lat'] = MUC[0] + 0.0001
        r = self.c.post('/api/v1/spots/bulk?upsert=1', json=body, headers=ADMIN)
        self.assertEqual(r.json['created'], 5)
        self.assertEqual(self.c.get('/api/v1/spots/B-0', headers=ADMIN).json['status'], 'free')

    # -- Gateway ------------------------------------------------------------

    def test_events(self):
        self.add('S-1')
        r = self.report('S-1', 'occupied')
        self.assertEqual(r.json['applied'], 1)
        self.assertTrue(r.json['results'][0]['changed'])
        self.assertEqual(self.c.get('/api/v1/spots/S-1', headers=ADMIN).json['status'], 'occupied')

        # gleicher Status noch einmal: uebernommen, aber kein Wechsel
        self.assertFalse(self.report('S-1', 'occupied').json['results'][0]['changed'])

    def test_events_unbekannt_und_ungueltig(self):
        r = self.c.post('/api/v1/events', headers=GW, json={'events': [
            {'sensor_id': 'GIBTSNICHT', 'status': 'free'},
            {'sensor_id': 'S-1', 'status': 'kaputt'},
            {'status': 'free'},
        ]})
        self.assertEqual(r.status_code, 200)            # Batch scheitert nie komplett
        self.assertEqual(r.json['applied'], 0)
        self.assertEqual([x['result'] for x in r.json['results']],
                         ['unknown_sensor', 'invalid', 'invalid'])

    def test_veraltete_meldung_wird_verworfen(self):
        self.add('S-1')
        now = time.time()
        self.report('S-1', 'free', ts=now)
        r = self.report('S-1', 'occupied', ts=now - 60)   # verspaeteter Nachzuegler
        self.assertEqual(r.json['results'][0]['result'], 'stale_ignored')
        self.assertEqual(self.c.get('/api/v1/spots/S-1', headers=ADMIN).json['status'], 'free')

    def test_iso_zeitstempel_und_uhrfehler(self):
        self.add('S-1')
        r = self.report('S-1', 'free', ts='2026-07-27T10:00:00+00:00')
        self.assertEqual(r.json['applied'], 1)
        # Sensoruhr Jahre in der Zukunft -> auf Empfangszeit gedeckelt
        self.report('S-1', 'occupied', ts=time.time() + 10 * 365 * 86400)
        ts = self.c.get('/api/v1/spots/S-1', headers=ADMIN).json['status_ts']
        self.assertLess(ts, time.time() + config.MAX_CLOCK_SKEW_S + 5)

    def test_einzelmeldung_ohne_liste(self):
        self.add('S-1')
        r = self.c.post('/api/v1/events', headers=GW,
                        json={'sensor_id': 'S-1', 'status': 'free'})
        self.assertEqual(r.json['applied'], 1)

    # -- Umkreissuche -------------------------------------------------------

    def test_nearby_sortiert_und_filtert(self):
        # 0 m, ~111 m, ~1113 m noerdlich
        self.add('S-nah', lat=MUC[0])
        self.add('S-mittel', lat=MUC[0] + 0.001)
        self.add('S-fern', lat=MUC[0] + 0.010)
        for sid in ('S-nah', 'S-mittel', 'S-fern'):
            self.report(sid, 'free')
        self.add('S-belegt', lat=MUC[0] + 0.0005)
        self.report('S-belegt', 'occupied')

        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500' % MUC)
        ids = [s['sensor_id'] for s in r.json['spots']]
        self.assertEqual(ids, ['S-nah', 'S-mittel'])     # sortiert, belegte raus
        self.assertLess(r.json['spots'][0]['distance_m'], 1)
        self.assertAlmostEqual(r.json['spots'][1]['distance_m'], 111, delta=3)

        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=2000' % MUC)
        self.assertEqual(r.json['count'], 3)

        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=2000&status=any' % MUC)
        self.assertEqual(r.json['count'], 4)

        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500&limit=1' % MUC)
        self.assertEqual((r.json['count'], r.json['total_in_radius']), (1, 2))

    def test_nearby_ignoriert_veraltete_und_abgeschaltete(self):
        self.add('S-alt')
        self.report('S-alt', 'free', ts=time.time() - config.STALE_AFTER_S - 60)
        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500' % MUC)
        self.assertEqual(r.json['count'], 0)
        # max_age=0 hebt die Pruefung auf
        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500&max_age=0' % MUC)
        self.assertEqual(r.json['count'], 1)
        self.assertTrue(r.json['spots'][0]['stale'])

        self.add('S-aus')
        self.report('S-aus', 'free')
        self.c.patch('/api/v1/spots/S-aus', json={'disabled': True}, headers=ADMIN)
        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500' % MUC)
        self.assertEqual([s['sensor_id'] for s in r.json['spots']], [])

    def test_nearby_liefert_navi_links(self):
        self.add('S-1')
        self.report('S-1', 'free')
        nav = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f' % MUC).json['spots'][0]['nav']
        self.assertIn('destination=48.135100,11.582000', nav['google'])
        self.assertIn('daddr=48.135100,11.582000', nav['apple'])
        self.assertTrue(nav['geo'].startswith('geo:'))

    # -- Richtungsabhaengige Suche ------------------------------------------

    def test_heading_bevorzugt_plaetze_voraus(self):
        # 50 m noerdlich (voraus) und 20 m suedlich (hinter mir)
        self.add('S-voraus', lat=MUC[0] + 0.00045)
        self.add('S-hinter', lat=MUC[0] - 0.00018)
        self.report('S-voraus', 'free')
        self.report('S-hinter', 'free')

        # Ohne heading gewinnt die Luftlinie: der naehere hinter mir
        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500' % MUC)
        self.assertEqual(r.json['spots'][0]['sensor_id'], 'S-hinter')
        self.assertEqual(r.json['query']['sorted_by'], 'distance_m')

        # Mit heading=0 (nach Norden) gewinnt der Platz voraus
        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500&heading=0' % MUC)
        erst, zweit = r.json['spots']
        self.assertEqual(erst['sensor_id'], 'S-voraus')
        self.assertTrue(erst['ahead'])
        self.assertFalse(zweit['ahead'])
        self.assertEqual(r.json['query']['sorted_by'], 'effective_m')
        # voraus: Fahrstrecke = Luftlinie; dahinter: 2x plus Wendekosten
        self.assertAlmostEqual(erst['effective_m'], erst['distance_m'], places=1)
        self.assertAlmostEqual(zweit['effective_m'],
                               2 * zweit['distance_m'] + 150, places=1)

    def test_heading_umgekehrt_dreht_die_reihenfolge(self):
        self.add('S-nord', lat=MUC[0] + 0.00045)
        self.add('S-sued', lat=MUC[0] - 0.00018)
        for sid in ('S-nord', 'S-sued'):
            self.report(sid, 'free')
        nach_sueden = self.c.get(
            '/api/v1/spots/nearby?lat=%f&lng=%f&radius=500&heading=180' % MUC)
        self.assertEqual(nach_sueden.json['spots'][0]['sensor_id'], 'S-sued')

    def test_strassenseite_kostet_zuschlag(self):
        # Zwei Luecken gleich weit voraus, eine auf der Gegenfahrbahn
        self.add('S-rechts', lat=MUC[0] + 0.0003, street_bearing=0)
        self.add('S-links', lat=MUC[0] + 0.0003, lng=MUC[1] + 0.00001,
                 street_bearing=180)
        for sid in ('S-rechts', 'S-links'):
            self.report(sid, 'free')

        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500&heading=0' % MUC)
        erst, zweit = r.json['spots']
        self.assertEqual(erst['sensor_id'], 'S-rechts')
        self.assertTrue(erst['same_side'])
        self.assertFalse(zweit['same_side'])
        self.assertAlmostEqual(zweit['effective_m'] - zweit['distance_m'], 80, places=1)

        # Zuschlag pro Anfrage einstellbar -- fuer Messreihen
        r = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&radius=500&heading=0&cross=0' % MUC)
        self.assertAlmostEqual(r.json['spots'][1]['effective_m'],
                               r.json['spots'][1]['distance_m'], places=1)

    def test_ohne_street_bearing_keine_seitenaussage(self):
        self.add('S-1', lat=MUC[0] + 0.0003)
        self.report('S-1', 'free')
        s = self.c.get('/api/v1/spots/nearby?lat=%f&lng=%f&heading=0' % MUC).json['spots'][0]
        self.assertIsNone(s['same_side'])

    def test_street_bearing_wird_normalisiert(self):
        self.add('S-1', street_bearing=450)          # 450 Grad == 90 Grad
        self.assertEqual(self.c.get('/api/v1/spots/S-1', headers=ADMIN).json['street_bearing'], 90)
        self.c.patch('/api/v1/spots/S-1', json={'street_bearing': None}, headers=ADMIN)
        self.assertIsNone(self.c.get('/api/v1/spots/S-1', headers=ADMIN).json['street_bearing'])

    # -- Ziel-Waechter ------------------------------------------------------

    def test_status_endpunkt_ohne_schluessel(self):
        self.add('S-1')
        self.report('S-1', 'occupied')
        r = self.c.get('/api/v1/spots/S-1/status')      # bewusst ohne Key
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json['status'], 'occupied')
        self.assertFalse(r.json['stale'])
        self.assertNotIn('lat', r.json)                 # keine Stammdaten
        self.assertEqual(self.c.get('/api/v1/spots/GIBTSNICHT/status').status_code, 404)

    # -- Verwaltungsseite ---------------------------------------------------

    def test_login(self):
        r = self.c.post('/api/v1/login',
                        json={'username': config.ADMIN_USER, 'password': config.ADMIN_PASSWORD})
        self.assertEqual(r.status_code, 200)
        token = r.json['token']
        # Der ausgehaendigte Token muss die Admin-Endpunkte auch wirklich oeffnen
        self.assertEqual(self.c.get('/api/v1/spots', headers={'X-API-Key': token}).status_code, 200)

        for schlecht in ({'username': 'admin', 'password': 'falsch'},
                         {'username': 'root', 'password': config.ADMIN_PASSWORD},
                         {'username': '', 'password': ''}):
            self.assertEqual(self.c.post('/api/v1/login', json=schlecht).status_code, 401)
        self.assertEqual(self.c.post('/api/v1/login', json={'username': 'admin'}).status_code, 400)

    def test_adminseite(self):
        self.assertEqual(self.c.get('/admin').status_code, 302)
        r = self.c.get('/admin/')
        self.assertEqual(r.status_code, 200)
        html = r.get_data(as_text=True)
        self.assertIn('Sensorverwaltung', html)
        # Die Seite darf weder Schluessel noch Passwort enthalten -- sie ist
        # nur die Huelle, beides kommt erst ueber /login bzw. den Benutzer.
        self.assertNotIn(config.ADMIN_KEY, html)
        self.assertNotIn(config.GATEWAY_KEY, html)
        self.assertNotIn(config.ADMIN_PASSWORD, html)

    # -- Auswertung ---------------------------------------------------------

    def test_history_und_stats(self):
        self.add('S-1', lot='P3')
        self.report('S-1', 'free')
        self.report('S-1', 'occupied')
        h = self.c.get('/api/v1/spots/S-1/history', headers=ADMIN).json
        self.assertEqual([e['status'] for e in h['events']], ['occupied', 'free'])
        self.assertEqual(h['events'][0]['gateway_id'], 'GW-TEST')

        s = self.c.get('/api/v1/stats', headers=ADMIN).json
        self.assertEqual(s['lots']['P3']['occupied'], 1)
        self.assertEqual(s['lots']['P3']['total'], 1)

    def test_health_und_index(self):
        self.assertTrue(self.c.get('/api/v1/health').json['ok'])
        self.assertIn('endpoints', self.c.get('/').json)
        self.assertEqual(self.c.get('/api/v1/gibtsnicht').status_code, 404)

    # -- Fahrer-App ---------------------------------------------------------

    def test_app_wird_ausgeliefert(self):
        # Ohne Schraegstrich muss umgeleitet werden, sonst laufen die relativen
        # Pfade der Seite gegen "/" und vendor/leaflet.js kommt als 404 zurueck.
        r = self.c.get('/app')
        self.assertEqual(r.status_code, 302)
        self.assertTrue(r.headers['Location'].endswith('/app/'))

        r = self.c.get('/app/')
        self.assertEqual(r.status_code, 200)
        html = r.get_data(as_text=True)
        self.assertIn('Freie Parkplätze', html)
        self.assertIn('/spots/nearby', html)          # ruft die richtige Route
        self.assertNotIn('http://', html.split('<script>')[0])  # keine Fremdinhalte

        self.assertEqual(self.c.get('/app/manifest.json').json['short_name'], 'Parken')
        self.assertEqual(self.c.get('/app/icon.svg').status_code, 200)
        # Karte liegt lokal bei, nicht auf einem CDN
        self.assertIn('vendor/leaflet.js', html)
        self.assertEqual(self.c.get('/app/vendor/leaflet.js').status_code, 200)
        self.assertEqual(self.c.get('/app/vendor/leaflet.css').status_code, 200)
        # Pfad-Ausbruch muss scheitern
        self.assertIn(self.c.get('/app/../config.py').status_code, (403, 404))


class GeoTest(unittest.TestCase):

    def test_haversine(self):
        self.assertAlmostEqual(haversine(48.1351, 11.5820, 48.1351, 11.5820), 0, places=6)
        self.assertAlmostEqual(haversine(48.1351, 11.5820, 48.1441, 11.5820), 1001, delta=5)
        # Muenchen - Berlin, ~504 km
        self.assertAlmostEqual(haversine(48.1351, 11.5820, 52.5200, 13.4050) / 1000,
                               504, delta=5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
