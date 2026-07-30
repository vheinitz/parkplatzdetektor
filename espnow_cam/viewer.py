#!/usr/bin/env python3
"""Empfaengt JPEG-Bilder von der ESP-NOW-Bridge und zeigt sie im Browser.

Die Bridge (ESP32-C3 am USB) schiebt jedes Bild als
    A5 5A A5 5A | uint32 Laenge (little endian) | JPEG
ueber die serielle Leitung. Dazwischen stehen Textzeilen mit Statistik, die
hier ignoriert werden.

Aufruf:  python3 viewer.py [--port /dev/ttyACM0] [--http 8080]
Dann:    http://localhost:8080
"""
import argparse
import http.server
import socketserver
import sys
import threading
import time

import serial

MAGIC = b'\xA5\x5A\xA5\x5A'

# Gemeinsamer Zustand zwischen Leser-Thread und Webserver
state = {
    'frame': None,       # letztes vollstaendiges JPEG
    'seq': 0,            # Zaehler, damit der Stream neue Bilder erkennt
    'count': 0,
    'bad': 0,
    'started': time.time(),
    'last': 0.0,
}
lock = threading.Lock()


def reader(port):
    """Liest den seriellen Strom und legt fertige Bilder in state ab."""
    s = serial.Serial()
    s.port = port
    s.baudrate = 921600          # bei native USB-CDC ohne Bedeutung
    s.timeout = 0.2
    # Wichtig: DTR/RTS vor dem Oeffnen abschalten. Beim ESP32-C3 steuern die
    # Leitungen ueber den USB-Serial-JTAG-Block Reset und Bootmodus — sonst
    # startet das Board beim Verbinden neu oder haengt im Bootloader.
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False
    print(f'[viewer] lese von {port}', flush=True)

    buf = bytearray()
    while True:
        chunk = s.read(8192)
        if chunk:
            buf += chunk
        elif not buf:
            continue

        # Solange ein vollstaendiger Rahmen im Puffer steckt, herausziehen.
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                # Kein Rahmenanfang: Puffer begrenzen, aber Teilmagic behalten
                if len(buf) > 3:
                    del buf[:-3]
                break
            if len(buf) < i + 8:
                del buf[:i]
                break
            length = int.from_bytes(buf[i + 4:i + 8], 'little')
            if length == 0 or length > 200000:
                del buf[:i + 4]          # unplausibel -> Magic ueberspringen
                with lock:
                    state['bad'] += 1
                continue
            end = i + 8 + length
            if len(buf) < end:
                del buf[:i]              # Rest abwarten
                break
            data = bytes(buf[i + 8:end])
            del buf[:end]
            if data[:2] == b'\xff\xd8' and data[-2:] == b'\xff\xd9':
                with lock:
                    state['frame'] = data
                    state['seq'] += 1
                    state['count'] += 1
                    state['last'] = time.time()
            else:
                with lock:
                    state['bad'] += 1


PAGE = b"""<!doctype html>
<title>ESP32-CAM ueber ESP-NOW</title>
<style>
 body{background:#111;color:#eee;font-family:system-ui,sans-serif;margin:0;
      display:flex;flex-direction:column;align-items:center;gap:12px;padding:16px}
 img{width:min(95vw,720px);image-rendering:pixelated;border:1px solid #333;background:#000}
 #s{font-variant-numeric:tabular-nums;font-size:14px;color:#9bb}
</style>
<h2>ESP32-CAM &rarr; ESP-NOW &rarr; USB</h2>
<img src="/stream">
<div id="s">...</div>
<script>
setInterval(async()=>{
  try{const r=await fetch('/stats');document.getElementById('s').textContent=await r.text();}
  catch(e){}
},1000);
</script>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        elif self.path == '/stats':
            with lock:
                n, bad, last = state['count'], state['bad'], state['last']
            up = time.time() - state['started']
            age = (time.time() - last) if last else -1
            body = (f'{n} Bilder empfangen | {bad} verworfen | '
                    f'{n/up:.1f} Bilder/s im Mittel | '
                    + (f'letztes vor {age:.1f} s' if age >= 0 else 'noch keins')
                    ).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path == '/snapshot':
            with lock:
                f = state['frame']
            if not f:
                self.send_error(503, 'noch kein Bild empfangen')
                return
            self.send_response(200)
            self.send_header('Content-Type', 'image/jpeg')
            self.send_header('Content-Length', str(len(f)))
            self.end_headers()
            self.wfile.write(f)

        elif self.path == '/stream':
            self.send_response(200)
            self.send_header('Content-Type',
                             'multipart/x-mixed-replace; boundary=frame')
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            seen = -1
            try:
                while True:
                    with lock:
                        seq, f = state['seq'], state['frame']
                    if f is not None and seq != seen:
                        seen = seq
                        self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\n')
                        self.wfile.write(b'Content-Length: %d\r\n\r\n' % len(f))
                        self.wfile.write(f)
                        self.wfile.write(b'\r\n')
                    else:
                        time.sleep(0.02)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_error(404)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--http', type=int, default=8080)
    a = ap.parse_args()

    t = threading.Thread(target=reader, args=(a.port,), daemon=True)
    t.start()

    with Server(('0.0.0.0', a.http), Handler) as srv:
        print(f'[viewer] http://localhost:{a.http}', flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print('\nEnde')


if __name__ == '__main__':
    main()
