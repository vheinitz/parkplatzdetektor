"""Zeichnet das Raster auf die Frames; freie Plaetze werden schwarz ausgemalt.

  python3 draw_grid.py                 # alle Frames -> overlay/
  python3 draw_grid.py frames/x.jpg out.png
"""
import json
import os
import sys

import cv2
import numpy as np

BASE = os.path.dirname(os.path.abspath(__file__))
GREEN, CYAN, YELLOW, RED, BLACK = (60, 220, 60), (255, 220, 0), (0, 235, 255), (0, 0, 255), (0, 0, 0)
MAGENTA = (255, 0, 200)


def draw(img, grid, labels=True, boxes=False):
    """Raster einzeichnen. Ist 'free' gesetzt, werden freie Plaetze schwarz gefuellt.

    boxes=True zeichnet zusaetzlich die YOLO-Fahrzeugboxen (Sichtprueflung).
    """
    ov = img.copy()
    for b in grid['blocks']:
        for c in b['columns']:
            x0, x1 = c['x_range']
            sm = c['seam_x']
            ln = c['row_lines_y']
            for a, z in ((x0, sm), (sm, x1)):
                for k in range(len(ln) - 1):
                    cv2.rectangle(ov, (a, int(round(ln[k]))), (z, int(round(ln[k + 1]))),
                                  GREEN, 1)
            cv2.line(ov, (sm, int(ln[0])), (sm, int(ln[-1])), YELLOW, 2)
            cv2.rectangle(ov, (x0, int(ln[0])), (x1, int(ln[-1])), CYAN, 2)
    out = cv2.addWeighted(ov, 0.75, img, 0.25, 0)

    # freie Plaetze deckend schwarz -- nach dem Blenden, sonst scheint das Bild durch
    for b in grid['blocks']:
        for c in b['columns']:
            if 'free' not in c:
                continue
            xs, sm, xe = c['x_range'][0], c['seam_x'], c['x_range'][1]
            ln = c['row_lines_y']
            for k, row in enumerate(c['free']):
                y0, y1 = int(round(ln[k])), int(round(ln[k + 1]))
                for free, (a, z) in zip(row, ((xs, sm), (sm, xe))):
                    if free:
                        cv2.rectangle(out, (a, y0), (z, y1), BLACK, -1)
                        cv2.rectangle(out, (a, y0), (z, y1), GREEN, 1)

    if boxes and grid.get('vehicles'):
        cv2.polylines(out, [np.array(p, np.int32) for p in grid['vehicles']],
                      True, MAGENTA, 1, cv2.LINE_AA)

    if labels:
        for b in grid['blocks']:
            for c in b['columns']:
                cv2.putText(out, f"B{b['block']}C{c['col']}:{c['n_slots']}",
                            (c['x_range'][0] + 2, int(c['row_lines_y'][0]) - 6),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, RED, 2, cv2.LINE_AA)
        if 'n_free' in grid:
            txt = f"frei {grid['n_free']} / {grid['total_slots']}"
            cv2.putText(out, txt, (12, 34), cv2.FONT_HERSHEY_SIMPLEX, 1.0, BLACK, 5, cv2.LINE_AA)
            cv2.putText(out, txt, (12, 34), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2,
                        cv2.LINE_AA)
    return out


def main():
    from occupancy import classify

    grid = json.load(open(os.path.join(BASE, 'grid.json')))
    if len(sys.argv) > 2:
        pairs = [(sys.argv[1], sys.argv[2])]
    else:
        src = os.path.join(BASE, 'frames')
        dst = os.path.join(BASE, 'overlay')
        os.makedirs(dst, exist_ok=True)
        pairs = [(os.path.join(src, f), os.path.join(dst, f))
                 for f in sorted(os.listdir(src)) if f.endswith('.jpg')]
    tot = 0
    for s, d in pairs:
        img = cv2.imread(s)
        if img is None:
            continue
        n_free, _ = classify(img, grid)
        tot += n_free
        cv2.imwrite(d, draw(img, grid))
    print(f'{len(pairs)} Bilder | {grid["total_slots"]} Stellplaetze | '
          f'im Mittel {tot / max(len(pairs), 1):.1f} frei')


if __name__ == '__main__':
    main()
