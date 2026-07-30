"""Konfiguration -- alles ueber Umgebungsvariablen, mit Vorgaben fuer lokal.

Auf PythonAnywhere werden die Werte in der WSGI-Datei gesetzt (siehe
wsgi_pythonanywhere.py), lokal reichen die Vorgaben.
"""
import os

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DB_PATH = os.environ.get('PARKING_DB', os.path.join(BASE_DIR, 'parking.sqlite3'))

# Zwei getrennte Schluessel: das Gateway darf nur Status melden, nie Stammdaten
# aendern. Wer die Gateway-Kennung abgreift, kann damit keine Plaetze loeschen.
ADMIN_KEY = os.environ.get('PARKING_ADMIN_KEY', 'dev-admin-key')
GATEWAY_KEY = os.environ.get('PARKING_GATEWAY_KEY', 'dev-gateway-key')
USING_DEV_KEYS = (ADMIN_KEY == 'dev-admin-key' or GATEWAY_KEY == 'dev-gateway-key')

# Anmeldung der Verwaltungsseite. Wer sich damit anmeldet, bekommt den
# Admin-Schluessel ausgehaendigt -- das Passwort ist also genauso schuetzenswert
# wie der Schluessel selbst.
ADMIN_USER = os.environ.get('PARKING_ADMIN_USER', 'admin')
ADMIN_PASSWORD = os.environ.get('PARKING_ADMIN_PASSWORD', 'adminpp')
USING_DEFAULT_PASSWORD = ADMIN_PASSWORD == 'adminpp'

DEFAULT_RADIUS_M = float(os.environ.get('PARKING_DEFAULT_RADIUS_M', 500))
MAX_RADIUS_M = float(os.environ.get('PARKING_MAX_RADIUS_M', 5000))
DEFAULT_LIMIT = int(os.environ.get('PARKING_DEFAULT_LIMIT', 20))
MAX_LIMIT = int(os.environ.get('PARKING_MAX_LIMIT', 200))

# --- Richtungsabhaengige Suche (nur wirksam, wenn die App "heading" mitschickt)
# Bei Parkluecken am Strassenrand ist die Luftlinie das falsche Mass: eine
# Luecke 30 m hinter mir ist praktisch weiter weg als eine 150 m voraus. Beides
# wird deshalb mit Zuschlaegen bewertet statt nur mit der Entfernung.

# Bis zu diesem Winkel neben der Fahrtrichtung gilt ein Platz als "voraus".
AHEAD_HALF_ANGLE = float(os.environ.get('PARKING_AHEAD_HALF_ANGLE', 90))

# Zuschlag fuer einen Platz hinter mir: einmal vorbei, wenden, zurueck.
U_TURN_M = float(os.environ.get('PARKING_U_TURN_M', 150))

# Zuschlag fuer die falsche Strassenseite (nur wenn street_bearing gepflegt ist).
CROSS_M = float(os.environ.get('PARKING_CROSS_M', 80))

# Meldet ein Sensor laenger als das nichts, gilt sein Wert als veraltet und
# fliesst nicht mehr in /nearby ein. Ein "frei" von gestern ist schlimmer als
# gar keine Angabe.
STALE_AFTER_S = float(os.environ.get('PARKING_STALE_AFTER_S', 900))

# Ereignis-Log fuer Statistik (enthaelt nur Sensor-ID, Status, Zeit -- keine
# Personenbezug). '0' schaltet es ab.
KEEP_EVENTS = os.environ.get('PARKING_KEEP_EVENTS', '1') not in ('0', 'false', 'no')

# Zeitstempel, die weiter als das in der Zukunft liegen, sind eine falsch
# gestellte Sensoruhr -- wird auf die Empfangszeit korrigiert.
MAX_CLOCK_SKEW_S = float(os.environ.get('PARKING_MAX_CLOCK_SKEW_S', 300))
