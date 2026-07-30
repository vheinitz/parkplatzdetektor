"""Sensor-Gateway simulieren -- Hardware-Ersatz fuer den Prototyp.

Bildet das Verhalten echter Sensoren nach: gemeldet wird **nur bei
Zustandswechsel**, gebuendelt je Gateway-Uplink, plus ein Heartbeat, damit der
Server stumme Sensoren von belegten unterscheiden kann.

    python3 simulate_gateway.py                                  # gegen localhost
    python3 simulate_gateway.py --url https://user.pythonanywhere.com --key GEHEIM
    python3 simulate_gateway.py --interval 5 --churn 0.1         # lebhafter

Nur Standardbibliothek -- laeuft auch auf einem Raspberry ohne Extras.
"""
import argparse
import json
import random
import time
import urllib.error
import urllib.request


def get(url, key=None):
    req = urllib.request.Request(url, headers={'X-API-Key': key} if key else {})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def post(url, key, payload):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={'Content-Type': 'application/json', 'X-API-Key': key})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--url', default='http://127.0.0.1:5000')
    ap.add_argument('--key', default='dev-gateway-key', help='Gateway-API-Key')
    ap.add_argument('--admin-key', default='dev-admin-key',
                    help='nur zum Einlesen der Sensorliste')
    ap.add_argument('--gateway-id', default='GW-SIM-1')
    ap.add_argument('--interval', type=float, default=10, help='Sekunden je Uplink')
    ap.add_argument('--churn', type=float, default=0.05,
                    help='Anteil der Plaetze, die je Runde wechseln')
    ap.add_argument('--heartbeat', type=float, default=300,
                    help='Sekunden, nach denen ein stummer Sensor sich meldet')
    ap.add_argument('--rounds', type=int, default=0, help='0 = endlos')
    args = ap.parse_args()
    base = args.url.rstrip('/') + '/api/v1'

    try:
        listing = get(base + '/spots?limit=1000', args.admin_key)
    except urllib.error.HTTPError as err:
        raise SystemExit('Sensorliste nicht lesbar (%s): %s'
                         % (err.code, err.read().decode(errors='replace')))
    except urllib.error.URLError as err:
        raise SystemExit('Server nicht erreichbar (%s): %s' % (args.url, err.reason))

    ids = [s['sensor_id'] for s in listing['spots']]
    if not ids:
        raise SystemExit('Keine Plaetze in der Datenbank -- erst  python3 seed.py  laufen lassen.')
    print('%d Sensoren, Uplink alle %.0f s' % (len(ids), args.interval))

    state = {i: random.choice(['free', 'occupied']) for i in ids}
    last_sent = {i: 0.0 for i in ids}
    # Store-and-forward wie bei echter Hardware: scheitert ein Uplink, bleiben
    # die Meldungen liegen und gehen beim naechsten mit. Je Sensor zaehlt nur
    # die letzte -- ein zwischendurch verworfener Zwischenstand interessiert
    # niemanden mehr.
    pending = {}
    rnd = random.Random(42)
    rounds = 0

    while args.rounds == 0 or rounds < args.rounds:
        now = time.time()

        # 1) echte Zustandswechsel
        n_change = max(1, int(len(ids) * args.churn))
        for sid in rnd.sample(ids, min(n_change, len(ids))):
            state[sid] = 'occupied' if state[sid] == 'free' else 'free'
            pending[sid] = {'sensor_id': sid, 'status': state[sid], 'ts': round(now, 1),
                            'battery': round(rnd.uniform(3.2, 3.7), 2),
                            'rssi': rnd.randint(-110, -70)}

        # 2) Heartbeat der stummen Sensoren -- sonst gilt ihr Wert als veraltet
        for sid in ids:
            if now - last_sent[sid] > args.heartbeat and sid not in pending:
                pending[sid] = {'sensor_id': sid, 'status': state[sid], 'ts': round(now, 1)}

        try:
            events = list(pending.values())
            res = post(base + '/events', args.key,
                       {'gateway_id': args.gateway_id, 'events': events})
            for ev in events:
                last_sent[ev['sensor_id']] = now
            pending.clear()
            n_free = sum(1 for v in state.values() if v == 'free')
            print('%s  %3d gesendet, %3d uebernommen, %3d frei'
                  % (time.strftime('%H:%M:%S'), res['received'], res['applied'], n_free))
            for r in res['results']:
                if r['result'] not in ('ok',):
                    print('   %s -> %s' % (r.get('sensor_id'), r['result']))
        except urllib.error.HTTPError as err:
            print('%s  Fehler %s: %s' % (time.strftime('%H:%M:%S'), err.code,
                                         err.read().decode(errors='replace')[:200]))
        except Exception as err:
            # Zeitueberschreitung, Verbindungsabbruch, kaputte Antwort: ein
            # einzelner Aussetzer darf den Dauerlauf nicht beenden. Frueher
            # rutschte TimeoutError hier durch -- es ist kein URLError.
            print('%s  Uplink fehlgeschlagen (%s: %s), %d Meldungen warten'
                  % (time.strftime('%H:%M:%S'), type(err).__name__, err, len(pending)))

        rounds += 1
        if args.rounds == 0 or rounds < args.rounds:
            time.sleep(args.interval)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print()
