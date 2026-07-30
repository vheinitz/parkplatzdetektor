"""SQLite-Zugriff: Schema, Verbindung je Request, kleine Helfer.

SQLite reicht fuer den Prototyp deutlich weiter als man denkt -- die Last ist
schreibarm (nur Zustandswechsel) und leseleicht. Wichtig sind zwei
Einstellungen: WAL, damit Lesen und Schreiben sich nicht blockieren, und ein
busy_timeout, damit gleichzeitige Requests warten statt zu scheitern.
"""
import sqlite3

from flask import g

import config

SCHEMA = """
CREATE TABLE IF NOT EXISTS spots (
    sensor_id   TEXT PRIMARY KEY,          -- Kennung des Sensors == Kennung des Platzes
    name        TEXT,                      -- z.B. "Ebene 2, Nr. 14"
    lot         TEXT,                      -- z.B. "Parkhaus P3"
    lat         REAL NOT NULL,
    lng         REAL NOT NULL,
    -- Richtung der Fahrbahn an dieser Luecke (0=Nord, 90=Ost), optional.
    -- Nur damit laesst sich sagen, ob ein Platz auf meiner Strassenseite liegt.
    street_bearing REAL,
    status      TEXT NOT NULL DEFAULT 'unknown',   -- free | occupied | unknown
    status_ts   REAL,                      -- Messzeit (vom Sensor/Gateway)
    updated_at  REAL NOT NULL,             -- Schreibzeit auf dem Server
    disabled    INTEGER NOT NULL DEFAULT 0 -- ausser Betrieb: wird nicht ausgeliefert
);
CREATE INDEX IF NOT EXISTS spots_lat_lng ON spots(lat, lng);
CREATE INDEX IF NOT EXISTS spots_status  ON spots(status);
CREATE INDEX IF NOT EXISTS spots_lot     ON spots(lot);

-- Ereignis-Log fuer Statistik. Bewusst ohne jeden Personenbezug: nur
-- "Sensor X war um 14:00 belegt". Kein Kennzeichen, keine Nutzer-ID.
CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    sensor_id   TEXT NOT NULL,
    status      TEXT NOT NULL,
    ts          REAL NOT NULL,             -- Messzeit
    received_at REAL NOT NULL,             -- Empfangszeit
    gateway_id  TEXT,
    battery     REAL,                      -- optional, Volt
    rssi        INTEGER                    -- optional, Funkpegel
);
CREATE INDEX IF NOT EXISTS events_sensor_ts ON events(sensor_id, ts);
"""

STATUSES = ('free', 'occupied', 'unknown')


def connect(path=None):
    """Neue Verbindung mit den Einstellungen, die SQLite hier braucht."""
    con = sqlite3.connect(path or config.DB_PATH, timeout=10)
    con.row_factory = sqlite3.Row
    con.execute('PRAGMA journal_mode=WAL')
    con.execute('PRAGMA synchronous=NORMAL')
    con.execute('PRAGMA busy_timeout=10000')
    return con


def get_db():
    """Verbindung des laufenden Requests (wird am Ende automatisch geschlossen)."""
    if 'db' not in g:
        g.db = connect()
    return g.db


def close_db(_exc=None):
    con = g.pop('db', None)
    if con is not None:
        con.close()


def init_db(path=None):
    """Schema anlegen -- idempotent, laeuft bei jedem Start."""
    con = connect(path)
    try:
        con.executescript(SCHEMA)
        # Nachtraeglich ergaenzte Spalten: CREATE TABLE IF NOT EXISTS laesst eine
        # bestehende Tabelle unangetastet, deshalb hier einzeln nachziehen.
        vorhanden = {r['name'] for r in con.execute('PRAGMA table_info(spots)')}
        for name, typ in (('street_bearing', 'REAL'),):
            if name not in vorhanden:
                con.execute('ALTER TABLE spots ADD COLUMN %s %s' % (name, typ))
        con.commit()
    finally:
        con.close()


def row_to_spot(row, now=None, stale_after=None):
    """DB-Zeile -> JSON-faehiges Dict, mit Alter und Veraltungs-Kennzeichen."""
    stale_after = config.STALE_AFTER_S if stale_after is None else stale_after
    age = None if row['status_ts'] is None or now is None else round(now - row['status_ts'], 1)
    return {
        'sensor_id': row['sensor_id'],
        'name': row['name'],
        'lot': row['lot'],
        'lat': row['lat'],
        'lng': row['lng'],
        'street_bearing': row['street_bearing'],
        'status': row['status'],
        'status_ts': row['status_ts'],
        'age_s': age,
        'stale': None if age is None else age > stale_after,
        'updated_at': row['updated_at'],
        'disabled': bool(row['disabled']),
    }
