"""LoRa-Gateway: serielle Zeilen vom Empfaenger -> POST /api/v1/events

Laeuft auf dem Rechner oder Raspberry, an dem der LoRa-Empfaenger per USB
haengt. Aufgabe: Zeilen lesen, Pruefsumme kontrollieren, Dubletten wegwerfen,
Zeitstempel setzen, buendeln und an den Server schicken.

    python3 gateway.py --port /dev/ttyACM0 --url https://user.pythonanywhere.com --key GEHEIM
    python3 gateway.py --port auto --dry-run          # nur anzeigen, nichts senden
    python3 gateway.py --replay mitschnitt.txt        # ohne Hardware, aus Datei

Warum der Zeitstempel hier entsteht und nicht im Sensor: der Sensor hat keine
Uhr. Eine Funkuhr oder ein RTC-Baustein je Parkluecke waere Aufwand und Strom
fuer nichts -- die Laufzeit ueber Funk liegt im Millisekundenbereich, und der
Server braucht den Zeitstempel nur, um veraltete Meldungen zu erkennen.

Warum die Warteschlange hier liegt und nicht im Mikrocontroller: faellt das
Internet aus, sammeln sich Meldungen an. Der Rechner hat Speicher dafuer, der
Mikrocontroller nicht. Je Sensor wird nur die **letzte** Meldung aufgehoben --
ein zwischendurch ueberholter Zwischenstand interessiert niemanden mehr.

Nur Standardbibliothek; pyserial wird erst beim Oeffnen eines echten Ports
gebraucht (pip3 install --user pyserial).
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

# Statusbuchstabe im Funkprotokoll -> Statuswort der Server-API.
# '?' meldet ein Sensor, der noch nicht kalibriert ist. Solche Meldungen
# werden gezaehlt, aber nicht weitergereicht: "weiss nicht" ist kein Zustand,
# den die App anzeigen koennte.
STATUS = {'F': 'free', 'B': 'occupied'}

DEDUP_TTL_S = 120.0


def crc16(daten):
    """CRC-16/CCITT-FALSE. Muss zu der Fassung im Sketch passen.

    Testvektor: b'123456789' -> 0x29B1.
    """
    crc = 0xFFFF
    for byte in daten:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def zerlege(zeile):
    """Eine serielle Zeile deuten.

    Rueckgabe (art, wert):
      ('kommentar', text)   Zeile mit '#' -- Statusmeldung der Bruecke
      ('paket', dict)       gueltiges Sensorpaket
      ('fehler', grund)     unbrauchbar, mit Begruendung fuer das Protokoll
      ('leer', None)
    """
    zeile = zeile.strip()
    if not zeile:
        return 'leer', None
    if zeile.startswith('#'):
        return 'kommentar', zeile.lstrip('# ')
    if zeile.startswith('PONG'):
        return 'kommentar', zeile

    teile = zeile.split(',')
    if teile[0] != 'RX':
        return 'fehler', 'kein RX: %r' % zeile[:60]
    # RX,PS1,<knoten>,<status>,<mV>,<seq>,<crc>,<rssi>,<snr>
    if len(teile) != 9:
        return 'fehler', '%d Felder statt 9: %r' % (len(teile), zeile[:60])

    _, magie, knoten, status, mv, seq, crc, rssi, snr = teile
    if magie != 'PS1':
        return 'fehler', 'fremdes Protokoll %r' % magie

    # Pruefsumme noch einmal pruefen, obwohl die Bruecke das schon getan hat.
    # Sie deckt hier zusaetzlich die USB-Strecke ab -- ein verschlucktes Byte
    # auf dem Kabel faellt sonst nirgends auf.
    rumpf = ','.join([magie, knoten, status, mv, seq])
    try:
        if crc16(rumpf.encode()) != int(crc, 16):
            return 'fehler', 'Pruefsumme falsch: %r' % zeile[:60]
    except ValueError:
        return 'fehler', 'Pruefsumme unlesbar: %r' % crc

    try:
        paket = {'knoten': knoten, 'status': status, 'mv': int(mv),
                 'seq': int(seq), 'rssi': int(rssi), 'snr': float(snr)}
    except ValueError:
        return 'fehler', 'Zahlenfeld unlesbar: %r' % zeile[:60]
    if status not in STATUS and status != '?':
        return 'fehler', 'unbekannter Status %r' % status
    return 'paket', paket


class Sammler:
    """Dubletten aussortieren und offene Meldungen aufheben.

    Ein Sensor sendet dasselbe Ereignis mehrfach, weil es auf dem Rueckweg
    keine Bestaetigung gibt (siehe carsensor.ino). Wiederholungen tragen
    dieselbe laufende Nummer und werden hier verworfen.
    """

    def __init__(self, karte=None, dedup_ttl=DEDUP_TTL_S):
        self.karte = karte or {}
        self.dedup_ttl = dedup_ttl
        self.gesehen = {}        # (knoten, seq) -> Zeitpunkt
        self.offen = {}          # sensor_id -> Ereignis
        self.zaehler = {'paket': 0, 'dublette': 0, 'unkalibriert': 0,
                        'fehler': 0, 'gesendet': 0}

    def sensor_id(self, knoten):
        """Ohne Zuordnungstabelle ist die Knotenkennung die Sensorkennung."""
        return self.karte.get(knoten, knoten)

    def aufnehmen(self, paket, jetzt):
        """True, wenn das Paket neu war und in die Warteschlange kam."""
        self.zaehler['paket'] += 1

        schluessel = (paket['knoten'], paket['seq'])
        # Abgelaufene Eintraege raus, sonst waechst die Tabelle unbegrenzt.
        # Der Ablauf hat einen zweiten Zweck: nach einem Neustart faengt der
        # Sensor wieder bei Nummer 1 an. Ohne Ablauf gaelte seine erste echte
        # Meldung als Dublette.
        for k, t in list(self.gesehen.items()):
            if jetzt - t > self.dedup_ttl:
                del self.gesehen[k]
        if schluessel in self.gesehen:
            self.gesehen[schluessel] = jetzt
            self.zaehler['dublette'] += 1
            return False
        self.gesehen[schluessel] = jetzt

        if paket['status'] not in STATUS:
            self.zaehler['unkalibriert'] += 1
            return False

        sid = self.sensor_id(paket['knoten'])
        ereignis = {'sensor_id': sid, 'status': STATUS[paket['status']],
                    'ts': round(jetzt, 1), 'rssi': paket['rssi']}
        if paket['mv'] > 0:
            ereignis['battery'] = round(paket['mv'] / 1000.0, 3)
        self.offen[sid] = ereignis
        return True

    def warteschlange(self):
        return list(self.offen.values())

    def bestaetigen(self, ereignisse):
        """Erst nach erfolgreichem Senden aufraeumen -- sonst gehen sie verloren."""
        for ev in ereignisse:
            # Kam waehrend des Sendens eine neuere Meldung, bleibt sie stehen.
            if self.offen.get(ev['sensor_id']) is ev:
                del self.offen[ev['sensor_id']]
        self.zaehler['gesendet'] += len(ereignisse)


def senden(basis, key, gateway_id, ereignisse):
    daten = json.dumps({'gateway_id': gateway_id, 'events': ereignisse}).encode()
    req = urllib.request.Request(
        basis.rstrip('/') + '/api/v1/events', data=daten,
        headers={'Content-Type': 'application/json', 'X-API-Key': key})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def port_suchen():
    """Den Port finden, an dem sich etwas als Gateway meldet."""
    try:
        import serial.tools.list_ports
    except ImportError:
        raise SystemExit('pyserial fehlt:  pip3 install --user pyserial')
    kandidaten = [p.device for p in serial.tools.list_ports.comports()]
    if not kandidaten:
        raise SystemExit('Keine seriellen Ports gefunden. Kabel? Rechte (dialout)?')
    print('Suche Gateway an: %s' % ', '.join(kandidaten))
    for dev in kandidaten:
        try:
            with oeffnen(dev, 115200) as ser:
                ser.write(b'PING\n')
                ende = time.time() + 3
                while time.time() < ende:
                    zeile = ser.readline().decode(errors='replace')
                    if 'PONG carsensor-gateway' in zeile:
                        print('  %s antwortet' % dev)
                        return dev
        except Exception as err:
            print('  %s: %s' % (dev, err))
    raise SystemExit('Kein Gateway gefunden. Sketch geflasht? Richtiger Port?')


def oeffnen(port, baud):
    try:
        import serial
    except ImportError:
        raise SystemExit('pyserial fehlt:  pip3 install --user pyserial')
    # DTR/RTS vor dem Oeffnen abschalten: sonst haelt pyserial manche Boards
    # im Reset (ESP32) oder im Bootloader (C3). Gleicher Griff wie im viewer
    # der Kamerastrecke.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def zeilen_seriell(port, baud):
    """Endlos Zeilen vom Empfaenger liefern; Abrisse werden ueberbrueckt."""
    while True:
        try:
            ser = oeffnen(port, baud)
        except SystemExit:
            raise
        except Exception as err:
            print('Port %s nicht offen (%s) -- neuer Versuch in 5 s' % (port, err))
            time.sleep(5)
            continue
        print('Verbunden mit %s' % port)
        ser.write(b'POLL\n')          # was ankam, bevor wir da waren
        try:
            while True:
                roh = ser.readline()
                if roh:
                    yield roh.decode(errors='replace')
                else:
                    yield ''          # Zeitueberlauf: Gelegenheit zum Senden
        except Exception as err:
            print('Serielle Verbindung verloren (%s) -- neuer Versuch' % err)
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(5)


def zeilen_datei(pfad):
    with open(pfad) as fh:
        for zeile in fh:
            yield zeile
    yield ''                          # letzte Runde ausloesen


def lauf(quelle, sammler, args, jetzt=time.time):
    """Kernschleife -- ohne Hardware und ohne Netz testbar."""
    letztes_senden = jetzt()
    for zeile in quelle:
        art, wert = zerlege(zeile)
        if art == 'paket':
            neu = sammler.aufnehmen(wert, jetzt())
            if args.verbose or neu:
                print('%s  %-12s %s  rssi %4d  snr %5.1f  seq %5d%s'
                      % (time.strftime('%H:%M:%S'), wert['knoten'], wert['status'],
                         wert['rssi'], wert['snr'], wert['seq'],
                         '' if neu else '  (Wiederholung)'))
        elif art == 'kommentar':
            print('  [bruecke] %s' % wert)
        elif art == 'fehler':
            sammler.zaehler['fehler'] += 1
            print('  [verworfen] %s' % wert)

        faellig = jetzt() - letztes_senden >= args.interval
        if sammler.warteschlange() and faellig:
            letztes_senden = jetzt()
            abgeben(sammler, args)

    if sammler.warteschlange():
        abgeben(sammler, args)


def abgeben(sammler, args):
    ereignisse = sammler.warteschlange()
    if args.dry_run:
        print('  [trocken] %d Meldung(en): %s'
              % (len(ereignisse), json.dumps(ereignisse, ensure_ascii=False)))
        sammler.bestaetigen(ereignisse)
        return
    try:
        res = args.sender(args.url, args.key, args.gateway_id, ereignisse)
    except urllib.error.HTTPError as err:
        print('  Server antwortet %s: %s -- %d Meldungen bleiben liegen'
              % (err.code, err.read().decode(errors='replace')[:200], len(ereignisse)))
        return
    except Exception as err:
        # Zeitueberlauf, DNS, abgebrochene Verbindung: der Dauerlauf darf
        # daran nicht sterben, und die Meldungen bleiben in der Warteschlange.
        print('  Senden fehlgeschlagen (%s: %s) -- %d Meldungen bleiben liegen'
              % (type(err).__name__, err, len(ereignisse)))
        return

    sammler.bestaetigen(ereignisse)
    print('  -> %d gesendet, %d uebernommen' % (res.get('received', 0), res.get('applied', 0)))
    for r in res.get('results', []):
        if r.get('result') == 'unknown_sensor':
            print('     %s ist dem Server unbekannt -- erst anlegen '
                  '(Verwaltungsseite oder POST /spots)' % r.get('sensor_id'))
        elif r.get('result') != 'ok':
            print('     %s -> %s' % (r.get('sensor_id'), r.get('result')))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='auto',
                    help='serieller Port, z.B. /dev/ttyACM0 oder COM3; "auto" sucht')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--url', default='http://127.0.0.1:5000', help='Basis-URL des Servers')
    ap.add_argument('--key', default='dev-gateway-key', help='Gateway-API-Key')
    ap.add_argument('--gateway-id', default='GW-LORA-1')
    ap.add_argument('--interval', type=float, default=5,
                    help='Sekunden, die Meldungen gebuendelt werden')
    ap.add_argument('--map', dest='karte', default=None, metavar='DATEI.json',
                    help='Zuordnung Knoten -> sensor_id, {"PS-0042": "RH-R03"}')
    ap.add_argument('--replay', default=None, metavar='DATEI',
                    help='Zeilen aus einer Datei statt vom Port lesen (ohne Hardware)')
    ap.add_argument('--dry-run', action='store_true', help='nichts an den Server senden')
    ap.add_argument('--verbose', action='store_true', help='auch Wiederholungen zeigen')
    args = ap.parse_args(argv)
    args.sender = senden

    karte = {}
    if args.karte:
        with open(args.karte) as fh:
            karte = json.load(fh)
        print('%d Zuordnungen aus %s' % (len(karte), args.karte))
    sammler = Sammler(karte)

    if args.replay:
        quelle = zeilen_datei(args.replay)
        print('Wiedergabe aus %s' % args.replay)
    else:
        port = port_suchen() if args.port == 'auto' else args.port
        quelle = zeilen_seriell(port, args.baud)

    ziel = 'nichts (--dry-run)' if args.dry_run else args.url
    print('Gateway %s -> %s, Buendelung %.0f s' % (args.gateway_id, ziel, args.interval))
    try:
        lauf(quelle, sammler, args)
    except KeyboardInterrupt:
        print()
    z = sammler.zaehler
    print('Pakete %d | Wiederholungen %d | unkalibriert %d | verworfen %d | gesendet %d'
          % (z['paket'], z['dublette'], z['unkalibriert'], z['fehler'], z['gesendet']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
