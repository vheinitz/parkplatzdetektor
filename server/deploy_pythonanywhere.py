"""Deployment auf PythonAnywhere ueber die REST-API -- ohne Konsole, ohne SSH.

Braucht nur einen API-Token (Account -> "API token"), nicht das Passwort. Ein
Token laesst sich einzeln widerrufen, ohne das Konto anzufassen.

Token bereitstellen -- eines von beiden:
    echo "<token>" > ~/.pythonanywhere_token && chmod 600 ~/.pythonanywhere_token
    export PA_TOKEN=<token>

    python3 deploy_pythonanywhere.py --check      # Token/Web-App pruefen
    python3 deploy_pythonanywhere.py --dry-run    # zeigt nur, was passieren wuerde
    python3 deploy_pythonanywhere.py              # hochladen, reload, /health
    python3 deploy_pythonanywhere.py --eu         # Konto auf den EU-Servern

Die Datenbank wird nie angefasst -- sie liegt eine Ebene ueber dem Code-Ordner.
Nur Standardbibliothek.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

# Genau diese Dateien machen den Server aus. parking.sqlite3 und __pycache__
# stehen bewusst nicht drin.
FILES = ['flask_app.py', 'db.py', 'geo.py', 'config.py',
         'seed.py', 'simulate_gateway.py', 'test_api.py', 'requirements.txt',
         'app/index.html', 'app/admin.html', 'app/manifest.json', 'app/icon.svg',
         'app/vendor/leaflet.js', 'app/vendor/leaflet.css']

TOKEN_FILE = os.path.expanduser('~/.pythonanywhere_token')
KEY_FILE = os.path.expanduser('~/.parkassistent_keys.json')
BOUNDARY = '----parkassistent-deploy-boundary'

WSGI_TEMPLATE = '''"""Erzeugt von deploy_pythonanywhere.py -- nicht von Hand aendern."""
import os
import sys

PROJEKT = %(dir)r

os.environ['PARKING_DB'] = %(db)r
os.environ['PARKING_ADMIN_KEY'] = %(admin)r
os.environ['PARKING_GATEWAY_KEY'] = %(gateway)r
os.environ['PARKING_STALE_AFTER_S'] = '900'

if PROJEKT not in sys.path:
    sys.path.insert(0, PROJEKT)

from flask_app import app as application
'''


def api_keys():
    """Admin- und Gateway-Schluessel -- einmal erzeugt, dann wiederverwendet.

    Sie landen in einer lokalen Datei, weil sie sonst beim naechsten Deploy neu
    wuerden und alle Gateways aussperren.
    """
    if os.path.exists(KEY_FILE):
        with open(KEY_FILE) as fh:
            return json.load(fh)
    import secrets
    keys = {'admin': secrets.token_urlsafe(32), 'gateway': secrets.token_urlsafe(32)}
    with open(KEY_FILE, 'w') as fh:
        json.dump(keys, fh, indent=2)
    os.chmod(KEY_FILE, 0o600)
    print('Neue API-Schluessel erzeugt und gespeichert in %s' % KEY_FILE)
    return keys


def read_token():
    token = os.environ.get('PA_TOKEN')
    if not token and os.path.exists(TOKEN_FILE):
        with open(TOKEN_FILE) as fh:
            token = fh.read().strip()
    if not token:
        raise SystemExit(
            'Kein API-Token. Auf https://www.pythonanywhere.com/account/#api_token\n'
            'erzeugen und dann:\n'
            '    echo "<token>" > %s && chmod 600 %s' % (TOKEN_FILE, TOKEN_FILE))
    return token


class Api:
    """Duenner Wrapper um die API v0."""

    def __init__(self, user, token, eu=False):
        host = 'eu.pythonanywhere.com' if eu else 'www.pythonanywhere.com'
        self.base = 'https://%s/api/v0/user/%s/' % (host, user)
        self.domain = '%s.%s' % (user, 'eu.pythonanywhere.com' if eu else 'pythonanywhere.com')
        self.token = token

    def call(self, path, method='GET', body=None, content_type=None):
        req = urllib.request.Request(self.base + path, data=body, method=method)
        req.add_header('Authorization', 'Token ' + self.token)
        if content_type:
            req.add_header('Content-Type', content_type)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                raw = resp.read()
                if not raw:
                    return resp.status, None
                try:
                    return resp.status, json.loads(raw)
                except ValueError:
                    return resp.status, raw.decode(errors='replace')
        except urllib.error.HTTPError as err:
            detail = err.read().decode(errors='replace')[:400]
            if err.code == 401:
                raise SystemExit('401 -- Token falsch oder abgelaufen.')
            if err.code == 403:
                raise SystemExit('403 -- Token gilt nicht fuer diesen Benutzer '
                                 '(oder EU/US vertauscht? --eu).')
            return err.code, detail
        except urllib.error.URLError as err:
            raise SystemExit('PythonAnywhere nicht erreichbar: %s' % err.reason)

    def upload(self, remote_path, data):
        """Datei schreiben. 201 = neu angelegt, 200 = ueberschrieben."""
        head = ('--%s\r\nContent-Disposition: form-data; name="content"; '
                'filename="%s"\r\nContent-Type: application/octet-stream\r\n\r\n'
                % (BOUNDARY, os.path.basename(remote_path))).encode()
        body = head + data + ('\r\n--%s--\r\n' % BOUNDARY).encode()
        return self.call('files/path' + remote_path, 'POST', body,
                         'multipart/form-data; boundary=' + BOUNDARY)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--user', default=os.environ.get('PA_USER'),
                    help='PythonAnywhere-Benutzername (oder $PA_USER)')
    ap.add_argument('--eu', action='store_true', help='Konto liegt auf den EU-Servern')
    ap.add_argument('--remote-dir', default=None,
                    help='Zielordner (Vorgabe /home/<user>/parkassistent/server)')
    ap.add_argument('--check', action='store_true', help='nur Token und Web-App pruefen')
    ap.add_argument('--wsgi', action='store_true',
                    help='WSGI-Datei erzeugen und mit hochladen (einmalig noetig)')
    ap.add_argument('--dry-run', action='store_true', help='nichts hochladen, nur zeigen')
    ap.add_argument('--no-reload', action='store_true', help='Web-App nicht neu laden')
    args = ap.parse_args()

    if not args.user:
        raise SystemExit('Benutzername fehlt:  --user <name>  oder  export PA_USER=<name>')

    api = Api(args.user, read_token(), args.eu)
    remote_dir = args.remote_dir or '/home/%s/parkassistent/server' % args.user
    here = os.path.dirname(os.path.abspath(__file__))

    # --- Zustand pruefen ---------------------------------------------------
    status, me = api.call('cpu/')
    if status != 200:
        raise SystemExit('Token wird nicht akzeptiert (%s): %s' % (status, me))
    print('Token ok fuer %s' % args.user)
    if isinstance(me, dict) and 'daily_cpu_limit_seconds' in me:
        print('  CPU heute: %.0f von %s s'
              % (me.get('daily_cpu_total_usage_seconds', 0), me['daily_cpu_limit_seconds']))

    status, webapps = api.call('webapps/')
    if status == 200 and isinstance(webapps, list):
        if not webapps:
            print('  WARNUNG: noch keine Web-App angelegt. Einmalig im Web-Tab:\n'
                  '           Add a new web app -> Manual configuration -> Python 3.x')
        for wa in webapps:
            print('  Web-App: %s  Python %s  %s'
                  % (wa.get('domain_name'), wa.get('python_version'),
                     'aktiv' if wa.get('enabled') else 'deaktiviert'))
    else:
        print('  Web-Apps nicht lesbar (%s): %s' % (status, webapps))

    if args.check:
        return

    # --- Dateien hochladen -------------------------------------------------
    print('\nZiel: %s' % remote_dir)
    fehler = 0
    for name in FILES:
        local = os.path.join(here, name)
        if not os.path.exists(local):
            print('  FEHLT lokal: %s' % name)
            fehler += 1
            continue
        if args.dry_run:
            print('  wuerde hochladen: %s (%d Bytes)' % (name, os.path.getsize(local)))
            continue
        with open(local, 'rb') as fh:
            data = fh.read()
        status, detail = api.upload(remote_dir + '/' + name, data)
        if status in (200, 201):
            print('  %-24s %s' % (name, 'neu' if status == 201 else 'aktualisiert'))
        else:
            print('  %-24s FEHLER %s: %s' % (name, status, detail))
            fehler += 1

    # --- WSGI-Datei (einmalig) --------------------------------------------
    if args.wsgi:
        keys = api_keys()
        wsgi = WSGI_TEMPLATE % {
            'dir': remote_dir,
            'db': os.path.dirname(remote_dir) + '/parking.sqlite3',
            'admin': keys['admin'],
            'gateway': keys['gateway'],
        }
        target = '/var/www/%s_wsgi.py' % api.domain.replace('.', '_')
        if args.dry_run:
            print('  wuerde schreiben: %s' % target)
        else:
            status, detail = api.upload(target, wsgi.encode())
            if status in (200, 201):
                print('  %-24s %s' % (os.path.basename(target),
                                      'neu' if status == 201 else 'aktualisiert'))
                print('  Schluessel stehen in %s' % KEY_FILE)
            else:
                print('  WSGI-Datei FEHLER %s: %s' % (status, detail))
                print('  -> Inhalt notfalls von Hand im Web-Tab einfuegen '
                      '(Vorlage: wsgi_pythonanywhere.py)')
                fehler += 1

    if args.dry_run:
        print('\n--dry-run: nichts geaendert.')
        return
    if fehler:
        print('\n%d Datei(en) nicht uebertragen -- kein Reload.' % fehler)
        sys.exit(1)

    # --- Web-App neu laden -------------------------------------------------
    if args.no_reload:
        print('\nReload uebersprungen.')
        return
    status, detail = api.call('webapps/%s/reload/' % api.domain, 'POST')
    if status not in (200, 201):
        print('\nReload fehlgeschlagen (%s): %s' % (status, detail))
        print('Web-App schon angelegt und WSGI-Datei eingetragen? Siehe README.')
        sys.exit(1)
    print('\nWeb-App neu geladen: https://%s' % api.domain)

    # --- Beweis, dass es laeuft --------------------------------------------
    url = 'https://%s/api/v1/health' % api.domain
    try:
        # Nach einem Reload braucht PythonAnywhere manchmal Sekunden, bis der
        # erste Aufruf durchkommt. Ein einzelner Zeitueberlauf heisst hier also
        # nicht, dass das Deployment kaputt ist.
        health = None
        for versuch in range(3):
            try:
                with urllib.request.urlopen(url, timeout=30) as resp:
                    health = json.load(resp)
                break
            except (TimeoutError, OSError) as err:
                if versuch == 2:
                    raise
                print('health noch nicht da (%s), neuer Versuch ...' % type(err).__name__)
        print('health: %s' % json.dumps(health, ensure_ascii=False))
        if health.get('warning'):
            print('ACHTUNG: %s -- eigene Schluessel in der WSGI-Datei setzen!'
                  % health['warning'])
    except urllib.error.HTTPError as err:
        print('health lieferte %s -- Fehlerlog im Web-Tab pruefen.' % err.code)
        sys.exit(1)
    except urllib.error.URLError as err:
        print('health nicht erreichbar: %s' % err.reason)
        sys.exit(1)


if __name__ == '__main__':
    main()
