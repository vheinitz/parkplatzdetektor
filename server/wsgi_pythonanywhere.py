"""Vorlage fuer die WSGI-Datei auf PythonAnywhere.

Diese Datei wird NICHT hier verwendet, sondern ihr Inhalt in
    /var/www/<benutzer>_pythonanywhere_com_wsgi.py
kopiert (Web-Tab -> "WSGI configuration file"). Alles, was dort vorher steht,
loeschen.

<benutzer> ueberall durch den eigenen PythonAnywhere-Benutzernamen ersetzen,
und die beiden Schluessel durch eigene Zufallswerte:

    python3 -c "import secrets; print(secrets.token_urlsafe(32))"
"""
import os
import sys

PROJEKT = '/home/<benutzer>/parkassistent/server'

# Umgebung setzen, BEVOR die App importiert wird -- config liest sie beim Import.
os.environ['PARKING_DB'] = '/home/<benutzer>/parkassistent/parking.sqlite3'
os.environ['PARKING_ADMIN_KEY'] = 'HIER-EIGENEN-ADMIN-SCHLUESSEL-EINTRAGEN'
os.environ['PARKING_GATEWAY_KEY'] = 'HIER-EIGENEN-GATEWAY-SCHLUESSEL-EINTRAGEN'
os.environ['PARKING_STALE_AFTER_S'] = '900'

if PROJEKT not in sys.path:
    sys.path.insert(0, PROJEKT)

from flask_app import app as application    # noqa: E402  -- den Namen erwartet WSGI
