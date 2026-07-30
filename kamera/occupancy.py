"""Belegt/frei je Stellplatz -- per YOLO-Fahrzeugerkennung.

Modellwahl: NICHT die COCO-Gewichte. COCO enthaelt praktisch keine
Draufsichten; das COCO-Modell haelt die Autos hier fuer "cell phone" und
findet null Fahrzeuge (auch 4x hochskaliert). Verwendet wird deshalb
yolo11*-obb, trainiert auf DOTA (Luftbilder) mit rotierten Boxen und den
Klassen "small vehicle" / "large vehicle" -- das passt zur Draufsicht und
liefert ~560 Fahrzeuge auf diesem Bild.

Zuordnung Detektion -> Zelle ueber die **Flaechenueberlappung**, nicht ueber
den Fahrzeugmittelpunkt. Grund: in der schraffierten Behindertenzone sitzt das
regelmaessige Raster leicht versetzt, dort faellt der Mittelpunkt in die
Nachbarzelle und die Zelle gilt faelschlich als frei (14 statt 6 freie Zellen
im Testbild). Die Ueberlappung ist gegen diesen Versatz robust.

Fahrende Autos auf den Fahrgassen zaehlen automatisch nicht mit: sie
ueberlappen keine Rasterzelle.
"""
import cv2
import numpy as np

MODEL = 'models/yolo11n-obb.pt'   # DOTA-Gewichte, rotierte Boxen
VEHICLE_CLASSES = (9, 10)         # large vehicle, small vehicle
CONF = 0.15
IMGSZ = 1024
MIN_OVERLAP = 0.20                # Anteil der Zellflaeche, ab dem sie belegt gilt

_cache = {}


def _model(name):
    """Modell einmalig laden und behalten -- der Ladevorgang dominiert sonst."""
    if name not in _cache:
        try:
            from ultralytics import YOLO
        except ImportError:
            raise SystemExit('ultralytics fehlt.  pip3 install --user ultralytics')
        _cache[name] = YOLO(name)
    return _cache[name]


def detect_vehicles(img, model=MODEL, conf=CONF, imgsz=IMGSZ, device='cpu'):
    """Fahrzeuge als rotierte Boxen (Liste von 4x2-Polygonen)."""
    r = _model(model).predict(img, imgsz=imgsz, conf=conf, device=device,
                              max_det=2000, verbose=False)[0]
    if r.obb is None:                       # falls doch ein Nicht-OBB-Modell
        return [np.array([[x0, y0], [x1, y0], [x1, y1], [x0, y1]], np.float32)
                for (x0, y0, x1, y1), c in zip(r.boxes.xyxy.tolist(), r.boxes.cls.tolist())
                if int(c) in VEHICLE_CLASSES]
    return [np.array(p, np.float32)
            for p, c in zip(r.obb.xyxyxyxy.tolist(), r.obb.cls.tolist())
            if int(c) in VEHICLE_CLASSES]


def slot_boxes(grid):
    """(block, col, row, side, x0, y0, x1, y1) fuer jeden Stellplatz."""
    for b in grid['blocks']:
        for c in b['columns']:
            xs, sm, xe = c['x_range'][0], c['seam_x'], c['x_range'][1]
            ln = c['row_lines_y']
            for k in range(len(ln) - 1):
                y0, y1 = int(round(ln[k])), int(round(ln[k + 1]))
                for side, (a, z) in (('L', (xs, sm)), ('R', (sm, xe))):
                    yield b['block'], c['col'], k, side, a, y0, z, y1


def _overlap(polys, x0, y0, x1, y1, stop_at):
    """Groesster Anteil der Zellflaeche, den ein Fahrzeug ueberdeckt."""
    rect = np.array([[x0, y0], [x1, y0], [x1, y1], [x0, y1]], np.float32)
    area = float((x1 - x0) * (y1 - y0))
    if area <= 0:
        return 0.0
    best = 0.0
    for p in polys:
        if (p[:, 0].max() < x0 or p[:, 0].min() > x1 or
                p[:, 1].max() < y0 or p[:, 1].min() > y1):
            continue                        # billiger Vorfilter
        inter, _ = cv2.intersectConvexConvex(rect, p)
        best = max(best, inter / area)
        if best >= stop_at:
            break
    return best


def classify(img, grid, model=MODEL, conf=CONF, imgsz=IMGSZ,
             min_overlap=MIN_OVERLAP, device='cpu'):
    """Traegt je Spalte 'free' ein ([links, rechts] pro Reihe). -> (frei, gesamt)."""
    polys = detect_vehicles(img, model=model, conf=conf, imgsz=imgsz, device=device)

    n_free = n_tot = 0
    for b in grid['blocks']:
        for c in b['columns']:
            xs, sm, xe = c['x_range'][0], c['seam_x'], c['x_range'][1]
            ln = c['row_lines_y']
            flags = []
            for k in range(len(ln) - 1):
                y0, y1 = int(round(ln[k])), int(round(ln[k + 1]))
                row = []
                for a, z in ((xs, sm), (sm, xe)):
                    free = _overlap(polys, a, y0, z, y1, min_overlap) < min_overlap
                    row.append(free)
                    n_tot += 1
                    n_free += free
                flags.append(row)
            c['free'] = flags

    grid['detector'] = {'model': model, 'conf': conf, 'imgsz': imgsz,
                        'min_overlap': min_overlap, 'n_vehicles': len(polys)}
    grid['vehicles'] = [np.round(p).astype(int).tolist() for p in polys]
    grid['n_free'] = n_free
    grid['n_occupied'] = n_tot - n_free
    return n_free, n_tot
