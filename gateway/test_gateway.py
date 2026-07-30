"""Tests fuer das Gateway -- ohne Funkmodul, ohne serielle Schnittstelle, ohne Server.

    python3 -m pytest test_gateway.py -q

Getestet wird die Strecke Zeile -> Paket -> Ereignis -> Server, also genau das,
was zwischen Funk und Netz passiert. Die Hardware liefert nur Zeilen; die
lassen sich hinschreiben.
"""
import types

import pytest

import gateway as gw


def zeile(knoten='PS-0042', status='F', mv=3712, seq=1, rssi=-97, snr=9.5, crc=None):
    """Eine serielle Zeile bauen, wie sie der Empfaenger ausgibt."""
    rumpf = 'PS1,%s,%s,%d,%d' % (knoten, status, mv, seq)
    if crc is None:
        crc = '%04X' % gw.crc16(rumpf.encode())
    return 'RX,%s,%s,%d,%.1f\n' % (rumpf, crc, rssi, snr)


def args_bauen(**abweichend):
    standard = dict(interval=0, dry_run=False, verbose=False, url='http://x',
                    key='k', gateway_id='GW-TEST', sender=None)
    standard.update(abweichend)
    return types.SimpleNamespace(**standard)


# --------------------------------------------------------------- Pruefsumme

def test_crc_testvektor():
    # Der Wert aus der CRC-Katalogliteratur. Steht auch im Sketch -- weichen
    # beide Fassungen voneinander ab, faellt es hier auf.
    assert gw.crc16(b'123456789') == 0x29B1


def test_crc_erkennt_verdrehte_zeichen():
    assert gw.crc16(b'PS1,PS-0042,F,3712,1') != gw.crc16(b'PS1,PS-0042,F,3712,2')


# --------------------------------------------------------------- Zerlegen

def test_gute_zeile():
    art, p = gw.zerlege(zeile())
    assert art == 'paket'
    assert p == {'knoten': 'PS-0042', 'status': 'F', 'mv': 3712,
                 'seq': 1, 'rssi': -97, 'snr': 9.5}


def test_kommentar_und_leerzeile():
    assert gw.zerlege('# bereit 868.0 MHz\n') == ('kommentar', 'bereit 868.0 MHz')
    assert gw.zerlege('PONG carsensor-gateway 1')[0] == 'kommentar'
    assert gw.zerlege('\n') == ('leer', None)


def test_falsche_pruefsumme_wird_verworfen():
    art, grund = gw.zerlege(zeile(crc='0000'))
    assert art == 'fehler' and 'Pruefsumme' in grund


def test_verstuemmelte_zeile():
    # Ein auf dem USB-Kabel verschlucktes Zeichen aendert den Rumpf, nicht die
    # mitgesendete Pruefsumme -- genau dafuer wird sie hier noch einmal geprueft.
    kaputt = zeile().replace('PS-0042', 'PS-004')
    assert gw.zerlege(kaputt)[0] == 'fehler'


def test_falsche_feldzahl():
    assert gw.zerlege('RX,PS1,PS-1,F,3712\n')[0] == 'fehler'


def test_fremdes_protokoll():
    assert gw.zerlege('RX,XX9,PS-1,F,3712,1,ABCD,-90,9.0\n')[0] == 'fehler'


def test_unbekannter_status():
    art, grund = gw.zerlege(zeile(status='X'))
    assert art == 'fehler' and 'Status' in grund


# --------------------------------------------------------------- Sammeln

def test_paket_wird_zu_ereignis():
    s = gw.Sammler()
    assert s.aufnehmen(gw.zerlege(zeile())[1], 1000.0) is True
    ev = s.warteschlange()[0]
    assert ev == {'sensor_id': 'PS-0042', 'status': 'free', 'ts': 1000.0,
                  'rssi': -97, 'battery': 3.712}


def test_belegt_wird_uebersetzt():
    s = gw.Sammler()
    s.aufnehmen(gw.zerlege(zeile(status='B'))[1], 1000.0)
    assert s.warteschlange()[0]['status'] == 'occupied'


def test_wiederholung_wird_verworfen():
    s = gw.Sammler()
    p = gw.zerlege(zeile(seq=7))[1]
    assert s.aufnehmen(p, 1000.0) is True
    assert s.aufnehmen(p, 1001.0) is False
    assert s.aufnehmen(p, 1002.0) is False
    assert len(s.warteschlange()) == 1
    assert s.zaehler['dublette'] == 2


def test_nach_neustart_des_sensors_zaehlt_seq_wieder_ab_eins():
    # Der Sensor haelt die laufende Nummer nur im RAM. Nach einem Neustart
    # faengt er wieder bei 1 an -- diese Meldung darf nicht als Wiederholung
    # untergehen, sonst bleibt eine gerade frei gewordene Luecke belegt.
    s = gw.Sammler(dedup_ttl=60)
    p = gw.zerlege(zeile(seq=1))[1]
    assert s.aufnehmen(p, 1000.0) is True
    assert s.aufnehmen(p, 1030.0) is False
    assert s.aufnehmen(p, 1200.0) is True


def test_unkalibrierter_sensor_meldet_nichts():
    s = gw.Sammler()
    assert s.aufnehmen(gw.zerlege(zeile(status='?'))[1], 1000.0) is False
    assert s.warteschlange() == []
    assert s.zaehler['unkalibriert'] == 1


def test_neuere_meldung_ersetzt_aeltere():
    s = gw.Sammler()
    s.aufnehmen(gw.zerlege(zeile(status='F', seq=1))[1], 1000.0)
    s.aufnehmen(gw.zerlege(zeile(status='B', seq=2))[1], 1001.0)
    assert len(s.warteschlange()) == 1
    assert s.warteschlange()[0]['status'] == 'occupied'


def test_zuordnungstabelle():
    s = gw.Sammler({'PS-0042': 'RH-R03'})
    s.aufnehmen(gw.zerlege(zeile())[1], 1000.0)
    assert s.warteschlange()[0]['sensor_id'] == 'RH-R03'


def test_fehlende_batteriespannung_bleibt_weg():
    s = gw.Sammler()
    s.aufnehmen(gw.zerlege(zeile(mv=-1))[1], 1000.0)
    assert 'battery' not in s.warteschlange()[0]


# --------------------------------------------------------------- Ganze Schleife

def test_lauf_sendet_gebuendelt():
    gesendet = []

    def sender(url, key, gid, ereignisse):
        gesendet.append(list(ereignisse))
        return {'received': len(ereignisse), 'applied': len(ereignisse), 'results': []}

    quelle = [zeile(knoten='A', seq=1), zeile(knoten='B', seq=1), '']
    s = gw.Sammler()
    gw.lauf(iter(quelle), s, args_bauen(sender=sender, interval=1000), jetzt=lambda: 1000.0)

    assert len(gesendet) == 1
    assert {e['sensor_id'] for e in gesendet[0]} == {'A', 'B'}
    assert s.warteschlange() == []


def test_ausfall_des_servers_verliert_nichts():
    # Der eigentliche Zweck der Warteschlange. Erst scheitert das Senden,
    # dann klappt es -- am Ende muessen beide Sensoren angekommen sein.
    versuche = []

    def sender(url, key, gid, ereignisse):
        versuche.append(list(ereignisse))
        # Der erste Durchlauf sendet zweimal: einmal faellig in der Schleife,
        # einmal beim Verlassen. Beide muessen scheitern, sonst prueft der
        # Test nicht, was er soll.
        if len(versuche) <= 2:
            raise TimeoutError('Server antwortet nicht')
        return {'received': len(ereignisse), 'applied': len(ereignisse), 'results': []}

    s = gw.Sammler()
    args = args_bauen(sender=sender, interval=0)
    gw.lauf(iter([zeile(knoten='A', seq=1)]), s, args, jetzt=lambda: 1000.0)
    assert s.warteschlange(), 'nach dem Fehlschlag muss die Meldung liegen bleiben'

    gw.lauf(iter([zeile(knoten='B', seq=1)]), s, args, jetzt=lambda: 1001.0)
    assert s.warteschlange() == []
    assert {e['sensor_id'] for e in versuche[-1]} == {'A', 'B'}


def test_http_fehler_beendet_den_lauf_nicht():
    import urllib.error

    def sender(url, key, gid, ereignisse):
        raise urllib.error.HTTPError(url, 401, 'Unauthorized', {}, None)

    s = gw.Sammler()
    gw.lauf(iter([zeile()]), s, args_bauen(sender=sender), jetzt=lambda: 1000.0)
    assert len(s.warteschlange()) == 1


def test_muell_zwischen_guten_zeilen():
    gesendet = []

    def sender(url, key, gid, ereignisse):
        gesendet.append(list(ereignisse))
        return {'received': 1, 'applied': 1, 'results': []}

    quelle = ['# bereit\n', 'kaputt\n', zeile(crc='0000'), zeile(knoten='C', seq=3), '']
    s = gw.Sammler()
    gw.lauf(iter(quelle), s, args_bauen(sender=sender, interval=1000), jetzt=lambda: 1000.0)
    assert s.zaehler['fehler'] == 2
    assert [e['sensor_id'] for e in gesendet[0]] == ['C']


def test_dry_run_sendet_nicht():
    def sender(*a):
        raise AssertionError('haette nicht senden duerfen')

    s = gw.Sammler()
    gw.lauf(iter([zeile(), '']), s, args_bauen(sender=sender, dry_run=True),
            jetzt=lambda: 1000.0)
    assert s.warteschlange() == []


def test_replay_aus_datei(tmp_path):
    pfad = tmp_path / 'mitschnitt.txt'
    pfad.write_text('# bereit\n' + zeile(knoten='D', seq=1))
    s = gw.Sammler()
    gw.lauf(gw.zeilen_datei(str(pfad)), s, args_bauen(dry_run=True), jetzt=lambda: 1000.0)
    assert s.zaehler['paket'] == 1


if __name__ == '__main__':
    raise SystemExit(pytest.main([__file__, '-q']))
