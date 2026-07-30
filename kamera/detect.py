#!/usr/bin/env python3
"""Prototyp: Bild rein -> Parkplatzraster erkennen, freie Plaetze zaehlen.

  python3 detect.py bild.jpg                  # -> bild_grid.png, zeigt es an
  python3 detect.py bild.jpg --no-show        # nur speichern
  python3 detect.py bild.jpg -o out.png       # Ziel selbst waehlen
  python3 detect.py bild.jpg --json g.json    # Geometrie + Belegung mitschreiben

Freie Plaetze werden schwarz ausgemalt. Das Raster wird in genau diesem Bild
gesucht, nichts wird vorausgesetzt.
"""
import argparse
import json
import os
import sys

import cv2

from draw_grid import draw
from fit_grid import fit, summary
from occupancy import CONF, IMGSZ, MIN_OVERLAP, MODEL, classify


def main():
    ap = argparse.ArgumentParser(description='Parkplatzraster erkennen, freie Plaetze zaehlen')
    ap.add_argument('image', help='Eingabebild (Draufsicht)')
    ap.add_argument('-o', '--out', help='Ausgabebild (Vorgabe: <name>_grid.png)')
    ap.add_argument('--json', help='Raster + Belegung als JSON speichern')
    ap.add_argument('--no-show', dest='show', action='store_false',
                    help='Ergebnis nicht anzeigen, nur speichern')
    ap.add_argument('--boxes', action='store_true',
                    help='erkannte Fahrzeuge als Boxen mitzeichnen')
    ap.add_argument('--model', default=MODEL, help=f'YOLO-Gewichte (Vorgabe {MODEL})')
    ap.add_argument('--conf', type=float, default=CONF,
                    help=f'YOLO-Konfidenzschwelle (Vorgabe {CONF})')
    ap.add_argument('--imgsz', type=int, default=IMGSZ,
                    help=f'YOLO-Eingabegroesse (Vorgabe {IMGSZ})')
    ap.add_argument('--min-overlap', type=float, default=MIN_OVERLAP,
                    help=f'Anteil der Zellflaeche, ab dem sie belegt gilt '
                         f'(Vorgabe {MIN_OVERLAP}; groesser = mehr gilt als frei)')
    ap.add_argument('--device', default='cpu', help="'cpu' oder z.B. '0' fuer GPU")
    ap.add_argument('--quiet', action='store_true', help='keine Rastertabelle drucken')
    args = ap.parse_args()

    img = cv2.imread(args.image)
    if img is None:
        sys.exit(f'Bild nicht lesbar: {args.image}')

    grid = fit(img)
    if not grid['total_slots']:
        sys.exit('Kein Raster gefunden -- ist das eine Parkplatz-Draufsicht?')
    grid['source_frame'] = os.path.basename(args.image)
    n_free, n_tot = classify(img, grid, model=args.model, conf=args.conf,
                             imgsz=args.imgsz, min_overlap=args.min_overlap,
                             device=args.device)

    out = args.out or os.path.splitext(args.image)[0] + '_grid.png'
    cv2.imwrite(out, draw(img, grid, boxes=args.boxes))
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(grid, f, indent=1)

    if not args.quiet:
        print(summary(grid))
    print(f"\nFahrzeuge erkannt: {grid['detector']['n_vehicles']}")
    print(f'FREI    {n_free:4d}')
    print(f'BELEGT  {n_tot - n_free:4d}')
    print(f'GESAMT  {n_tot:4d}   ({100.0 * (n_tot - n_free) / n_tot:.1f} % belegt)')
    print(f'-> {out}')

    if args.show:
        vis = cv2.imread(out)
        h = 900
        if vis.shape[0] > h:
            vis = cv2.resize(vis, (int(vis.shape[1] * h / vis.shape[0]), h))
        try:
            cv2.imshow('Parkplatz (Taste schliesst)', vis)
            cv2.waitKey(0)
            cv2.destroyAllWindows()
        except cv2.error:
            print('(Anzeige nicht moeglich -- kein GUI. Bild liegt unter obigem Pfad.)')


if __name__ == '__main__':
    main()
