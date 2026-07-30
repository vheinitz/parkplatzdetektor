"""Geo-Rechnung: Umkreissuche und Navi-Links.

Zweistufig gesucht wird aus Kostengruenden: erst ein Bounding-Box-Vorfilter im
SQL (nutzt den Index auf lat/lng), dann die exakte Haversine-Distanz in Python
nur noch auf den paar Kandidaten. Ein Kreis in Grad-Koordinaten laesst sich
nicht indizieren, ein Rechteck schon.
"""
import math

EARTH_R = 6371008.8          # mittlerer Erdradius in m (WGS84)
M_PER_DEG_LAT = 111320.0     # Breitengrad ist ueberall gleich lang


def haversine(lat1, lng1, lat2, lng2):
    """Entfernung zweier Punkte in Metern."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lng2 - lng1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_R * math.asin(min(1.0, math.sqrt(a)))


def peilung(lat1, lng1, lat2, lng2):
    """Kompasspeilung von Punkt 1 nach Punkt 2 in Grad. 0 = Nord, 90 = Ost."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lng2 - lng1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def winkel_diff(a, b):
    """Vorzeichenbehaftete Differenz a - b, zusammengefaltet auf (-180, 180].

    Damit ist 350 Grad und 10 Grad nur 20 Grad auseinander, nicht 340.
    """
    return (a - b + 540.0) % 360.0 - 180.0


def bbox(lat, lng, radius_m):
    """Umschliessendes Rechteck (lat_min, lat_max, lng_min, lng_max).

    Der Laengengrad schrumpft mit cos(Breite); nahe den Polen wird der Faktor
    unbrauchbar, dann wird auf den vollen Laengenbereich aufgemacht.
    """
    dlat = radius_m / M_PER_DEG_LAT
    cos_lat = math.cos(math.radians(lat))
    if abs(cos_lat) < 1e-6:
        dlng = 180.0
    else:
        dlng = min(180.0, radius_m / (M_PER_DEG_LAT * cos_lat))
    return (lat - dlat, lat + dlat, lng - dlng, lng + dlng)


def nav_links(lat, lng):
    """Deep-Links fuer die Standard-Navi-App -- ein Ziel, kein eigenes Routing.

    Genau so uebergeben es alle vergleichbaren Apps: die Auswahl passiert in der
    eigenen App, die Route macht das Betriebssystem.
    """
    pos = '%.6f,%.6f' % (lat, lng)
    return {
        'google': 'https://www.google.com/maps/dir/?api=1&destination=' + pos,
        'apple': 'http://maps.apple.com/?daddr=' + pos,
        'geo': 'geo:%s?q=%s' % (pos, pos),
    }
