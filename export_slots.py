"""Schneidet aus jedem Frame die einzelnen Stellplaetze aus  ->  slots/

Dateiname:  f<frame>_b<block>c<spalte>r<reihe><L|R>.jpg
Dazu slots/index.csv mit Herkunft, Bildkoordinaten, Linienkonfidenz und der
automatisch geschaetzten Belegung -- so laesst sich nach dem Labeln jederzeit
zurueckverfolgen, woher ein Crop kam.

Die Spalte 'free' ist ein *Vorschlag* des groben Schwellwertverfahrens aus
occupancy.py, kein geprueftes Label. Zum Anlernen nachkontrollieren.

  python3 export_slots.py [--margin 6] [--every 1] [--min-conf 0.0]
"""
import argparse
import csv
import json
import os

import cv2

from occupancy import classify

BASE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--margin', type=int, default=6,
                    help='Rand in px um den Stellplatz (Autos ueberhaengen die Linie)')
    ap.add_argument('--every', type=int, default=1, help='nur jeden n-ten Frame')
    ap.add_argument('--min-conf', type=float, default=0.0,
                    help='Stellplaetze ueberspringen, deren Randlinien unsicher sind')
    args = ap.parse_args()

    grid = json.load(open(os.path.join(BASE, 'grid.json')))
    src = os.path.join(BASE, 'frames')
    dst = os.path.join(BASE, 'slots')
    os.makedirs(dst, exist_ok=True)
    frames = sorted(f for f in os.listdir(src) if f.endswith('.jpg'))[::args.every]

    n = 0
    with open(os.path.join(dst, 'index.csv'), 'w', newline='') as fh:
        wr = csv.writer(fh)
        wr.writerow(['file', 'frame', 'block', 'col', 'row', 'side',
                     'x0', 'y0', 'x1', 'y1', 'line_conf', 'free'])
        for fn in frames:
            img = cv2.imread(os.path.join(src, fn))
            if img is None:
                continue
            H, W = img.shape[:2]
            classify(img, grid)
            stem = os.path.splitext(fn)[0].replace('frame_', '')
            for b in grid['blocks']:
                for c in b['columns']:
                    xs, sm, xe = c['x_range'][0], c['seam_x'], c['x_range'][1]
                    ln, lc = c['row_lines_y'], c['line_conf']
                    for k in range(len(ln) - 1):
                        conf = min(lc[k], lc[k + 1])
                        if conf < args.min_conf:
                            continue
                        for si, (side, (a, z)) in enumerate((('L', (xs, sm)), ('R', (sm, xe)))):
                            x0 = max(a - args.margin, 0)
                            x1 = min(z + args.margin, W)
                            y0 = max(int(round(ln[k])) - args.margin, 0)
                            y1 = min(int(round(ln[k + 1])) + args.margin, H)
                            crop = img[y0:y1, x0:x1]
                            if crop.size == 0:
                                continue
                            name = f"f{stem}_b{b['block']}c{c['col']}r{k:02d}{side}.jpg"
                            cv2.imwrite(os.path.join(dst, name), crop,
                                        [cv2.IMWRITE_JPEG_QUALITY, 95])
                            wr.writerow([name, fn, b['block'], c['col'], k, side,
                                         x0, y0, x1, y1, round(conf, 3),
                                         int(c['free'][k][si])])
                            n += 1
    print(f'{n} Stellplatz-Ausschnitte aus {len(frames)} Frames  ->  {dst}')


if __name__ == '__main__':
    main()
