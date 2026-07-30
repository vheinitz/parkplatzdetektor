"""Demo-Stammdaten anlegen: ein Raster von Stellplaetzen um einen Mittelpunkt.

Ersetzt spaeter das Einmessen der echten Sensoren. Die Masse entsprechen
ueblichen Buchten: 2,5 m breit, 5 m tief, dazwischen eine Fahrgasse von 6 m.

    python3 seed.py                                   # 40 Plaetze in Muenchen
    python3 seed.py --center 49.87,8.65 --rows 4 --cols 20 --lot "P1"
    python3 seed.py --url https://user.pythonanywhere.com --key GEHEIM
"""
import argparse
import json
import math
import time
import urllib.error
import urllib.request

M_PER_DEG_LAT = 111320.0


def build(center_lat, center_lng, rows, cols, lot, prefix, bay_w=2.5, bay_d=5.0, aisle=6.0):
    """Raster als Liste von Platz-Dicts. Reihen paarweise Ruecken an Ruecken."""
    m_per_deg_lng = M_PER_DEG_LAT * math.cos(math.radians(center_lat))
    spots = []
    for r in range(rows):
        # Reihenpaare: 0,1 liegen aneinander, danach kommt die Fahrgasse
        north = (r // 2) * (2 * bay_d + aisle) + (r % 2) * bay_d
        for c in range(cols):
            east = c * bay_w
            spots.append({
                'sensor_id': '%s-%02d-%03d' % (prefix, r, c),
                'name': 'Reihe %d, Platz %d' % (r + 1, c + 1),
                'lot': lot,
                'lat': round(center_lat + north / M_PER_DEG_LAT, 7),
                'lng': round(center_lng + east / m_per_deg_lng, 7),
            })
    return spots


def build_strasse(punkte, count, lot, prefix, beide_seiten, seitenversatz=4.0):
    """Parkluecken laengs eines Strassenzugs. punkte: Liste von (lat, lng).

    Fuer Strassenparken statt Parkplatz: die Luecken liegen aufgereiht am
    Fahrbahnrand, und jede bekommt die **Fahrbahnrichtung des Teilstuecks** mit,
    an dem sie liegt. Erst damit kann der Server sagen, ob ein Platz auf meiner
    Strassenseite liegt -- und bei einer Kurve stimmt das nur, wenn die Richtung
    lokal genommen wird und nicht ueber die ganze Strasse gemittelt.

    Zwei Stuetzpunkte ergeben die Gerade von frueher, viele den echten Verlauf.
    """
    if len(punkte) < 2:
        raise ValueError('mindestens zwei Stuetzpunkte noetig')
    lat0 = sum(p[0] for p in punkte) / len(punkte)
    m_lng = M_PER_DEG_LAT * math.cos(math.radians(lat0))
    ursprung = punkte[0]

    # In ein lokales Meter-Koordinatensystem umrechnen (x = Ost, y = Nord).
    xy = [((p[1] - ursprung[1]) * m_lng, (p[0] - ursprung[0]) * M_PER_DEG_LAT)
          for p in punkte]

    # Teilstuecke mit Laenge und Startdistanz -- daraus laesst sich jeder Punkt
    # entlang der Strasse direkt aufsuchen.
    stuecke, gesamt = [], 0.0
    for a, b in zip(xy, xy[1:]):
        laenge = math.hypot(b[0] - a[0], b[1] - a[1])
        if laenge < 1e-6:
            continue
        stuecke.append((a, b, laenge, gesamt))
        gesamt += laenge
    if not stuecke:
        raise ValueError('Strassenzug hat keine Laenge')

    spots = []
    for i in range(count):
        s = gesamt * (i + 0.5) / count              # Abstand entlang der Strasse
        for a, b, laenge, start in stuecke:
            if s <= start + laenge or (a, b, laenge, start) is stuecke[-1]:
                break
        u = min(1.0, (s - start) / laenge)
        px, py = a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u
        rx, ry = (b[0] - a[0]) / laenge, (b[1] - a[1]) / laenge     # Fahrtrichtung
        kurs = (math.degrees(math.atan2(rx, ry)) + 360) % 360
        qx, qy = ry, -rx                                            # quer, nach rechts

        for seite, vz, richtung in (('R', 1, kurs), ('L', -1, (kurs + 180) % 360)):
            if seite == 'L' and not beide_seiten:
                continue
            ost = px + qx * seitenversatz * vz
            nord = py + qy * seitenversatz * vz
            spots.append({
                'sensor_id': '%s-%s%02d' % (prefix, seite, i),
                'name': 'Lücke %d %s' % (i + 1, 'rechts' if seite == 'R' else 'links'),
                'lot': lot,
                'lat': round(ursprung[0] + nord / M_PER_DEG_LAT, 7),
                'lng': round(ursprung[1] + ost / m_lng, 7),
                'street_bearing': round(richtung, 1),
            })
    return spots


def post(url, key, payload):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={'Content-Type': 'application/json', 'X-API-Key': key})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--url', default='http://127.0.0.1:5000', help='Basis-URL des Servers')
    ap.add_argument('--key', default='dev-admin-key', help='Admin-API-Key')
    ap.add_argument('--center', default='48.1351,11.5820', help='lat,lng der Ecke unten links')
    ap.add_argument('--rows', type=int, default=4)
    ap.add_argument('--cols', type=int, default=10)
    ap.add_argument('--lot', default='Demo-Parkplatz')
    ap.add_argument('--prefix', default='S', help='Praefix der Sensor-IDs')
    ap.add_argument('--along', default=None, metavar='lat,lng:lat,lng[:...]',
                    help='Strassenparken: Luecken laengs dieses Strassenzugs statt Raster')
    ap.add_argument('--along-file', default=None, metavar='DATEI.json',
                    help='wie --along, aber Stuetzpunkte als JSON-Liste [[lat,lng],...]')
    ap.add_argument('--count', type=int, default=12, help='Luecken je Seite bei --along')
    ap.add_argument('--both-sides', action='store_true',
                    help='auch die Gegenseite der Strasse belegen')
    ap.add_argument('--dry-run', action='store_true', help='nur ausgeben, nicht senden')
    args = ap.parse_args()

    if args.along or args.along_file:
        if args.along_file:
            with open(args.along_file) as fh:
                punkte = [tuple(float(v) for v in p) for p in json.load(fh)]
        else:
            punkte = [tuple(float(v) for v in teil.split(','))
                      for teil in args.along.split(':')]
        spots = build_strasse(punkte, args.count, args.lot, args.prefix, args.both_sides)
        lat, lng = punkte[0]
    else:
        lat, lng = (float(x) for x in args.center.split(','))
        spots = build(lat, lng, args.rows, args.cols, args.lot, args.prefix)

    if args.dry_run:
        print(json.dumps({'spots': spots}, indent=2, ensure_ascii=False))
        return

    url = args.url.rstrip('/') + '/api/v1/spots/bulk?upsert=1'
    try:
        res = post(url, args.key, {'spots': spots})
    except urllib.error.HTTPError as err:
        raise SystemExit('Fehler %s: %s' % (err.code, err.read().decode(errors='replace')))
    except urllib.error.URLError as err:
        raise SystemExit('Server nicht erreichbar (%s): %s' % (args.url, err.reason))

    bad = [r for r in res['results'] if r['result'] != 'ok']
    print('%d von %d Plaetzen angelegt' % (res['created'], res['received']))
    for r in bad[:10]:
        print('  Fehler bei %s: %s' % (r['sensor_id'], r['detail']))

    # Startzustand melden, damit /nearby sofort etwas liefert
    now = time.time()
    events = [{'sensor_id': s['sensor_id'],
               'status': 'free' if i % 3 == 0 else 'occupied', 'ts': now}
              for i, s in enumerate(spots)]
    res = post(args.url.rstrip('/') + '/api/v1/events', args.key,
               {'gateway_id': 'seed', 'events': events})
    print('%d Statusmeldungen uebernommen' % res['applied'])
    print('Probe:  %s/api/v1/spots/nearby?lat=%s&lng=%s&radius=300'
          % (args.url.rstrip('/'), lat, lng))


if __name__ == '__main__':
    main()
