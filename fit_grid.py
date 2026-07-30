"""Parkplatz-Raster aus einer Drohnen-Draufsicht fitten.

  fit(img) -> dict           # als Bibliothek, siehe detect.py
  python3 fit_grid.py        # frames/frame_0001.jpg -> grid.json

Modell: 2 Bloecke x 5 Spalten; jede Spalte besteht aus 2 Unterspalten, die
Ruecken an Ruecken stehen (getrennt durch die Laengslinie = "Naht").
Quer dazu die Stellplatzlinien.

Ablauf pro Spalte:
  1. Kamm-Fit (Teilung + Phase) auf der Markierungsantwort. Ein starrer Kamm
     ist noetig, weil einzelne Linien unter parkenden Autos unsichtbar sind --
     Peak-Suche allein findet nur ~80 % und verschaetzt sich in der Teilung.
     Der Kamm wird ueber die Blockgrenze hinaus gespannt, damit die Randreihen
     nicht abgeschnitten werden.
  2. Lokales Einrasten auf die tatsaechlichen Markierungen; der Restfehler wird
     linear ueber die Reihen modelliert -> faengt leichte Perspektivdrift ab.
  3. Randreihen verwerfen, deren Mitte ausserhalb des Blocks liegt oder die nur
     Asphalt enthalten (die Fahrgassen zaehlen nicht mit).

Fahrende Autos auf den Fahrgassen spielen keine Rolle: gefittet wird die
Geometrie der Markierungen, und die Fahrgassen liegen ausserhalb der Spalten.
"""
import json
import os

import cv2
import numpy as np

from markings import responses

BASE = os.path.dirname(os.path.abspath(__file__))

PITCH_RANGE = (30.5, 33.5)   # plausible Reihenteilung in px
SNAP_PX = 7                  # max. Korrektur beim Einrasten
N_BLOCKS = 2                 # Bloecke, durch die quer laufende Fahrgasse getrennt


def runs(mask, minlen):
    """Zusammenhaengende True-Bereiche mit Mindestlaenge."""
    out, s = [], None
    for i, v in enumerate(list(mask) + [False]):
        if v and s is None:
            s = i
        elif not v and s is not None:
            if i - s >= minlen:
                out.append((s, i - 1))
            s = None
    return out


def sample(prof, ys):
    """Profil linear interpoliert an den Stellen ys."""
    ys = np.clip(np.asarray(ys, float), 0, len(prof) - 1.001)
    i0 = ys.astype(int)
    fr = ys - i0
    return prof[i0] * (1 - fr) + prof[i0 + 1] * fr


def fit(img):
    """Raster in einem Bild finden. Gibt das gleiche dict zurueck wie grid.json."""
    H, W = img.shape[:2]
    resp_h, resp_v = responses(img)

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY).astype(np.float32)
    mag = cv2.GaussianBlur(np.abs(cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3)) +
                           np.abs(cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3)), (0, 0), 3)

    # --- Bloecke und Spalten ueber Struktur-Energie ----------------------
    blocks = sorted(sorted(runs(mag.mean(axis=1) > 50, 200),
                           key=lambda r: r[1] - r[0], reverse=True)[:N_BLOCKS])
    cols_per_block = [runs(mag[y0:y1 + 1].mean(axis=0) > 60, 60) for (y0, y1) in blocks]

    grid = {
        'image_size': [W, H],
        'conf_hint': 'line_conf = Markierungsstaerke der Rasterlinie, 0..1 auf den '
                     'Median der Spalte normiert. Kleine Werte = Linie war verdeckt '
                     'oder das regelmaessige Raster passt dort nicht.',
        'blocks': [],
    }

    for bi, (by0, by1) in enumerate(blocks):
        bl = {'block': bi, 'y_range': [int(by0), int(by1)], 'columns': []}
        for ci, (x0, x1) in enumerate(cols_per_block[bi]):
            # 1) Kamm-Fit auf dem Blockbereich
            prof_b = np.percentile(resp_h[by0:by1 + 1, x0:x1 + 1], 85, axis=1).astype(np.float32)
            Lb = len(prof_b)
            _, pitch, ph0 = max(
                ((float(sample(prof_b, ph + np.arange(int((Lb - 1 - ph) // p) + 1) * p).mean()), p, ph)
                 for p in np.arange(PITCH_RANGE[0], PITCH_RANGE[1] + 1e-9, 0.025)
                 for ph in np.arange(0, PITCH_RANGE[1], 0.25)), key=lambda t: t[0])

            # Kamm ueber die Blockgrenze hinaus spannen
            ey0 = max(int(by0 - 1.5 * pitch), 0)
            ey1 = min(int(by1 + 1.5 * pitch), H - 1)
            prof = np.percentile(resp_h[ey0:ey1 + 1, x0:x1 + 1], 85, axis=1).astype(np.float32)
            L = len(prof)
            ph = ph0 + (by0 - ey0)
            ks = np.arange(int(np.floor(-ph / pitch)), int(np.ceil((L - 1 - ph) / pitch)) + 1)
            ys = ph + ks * pitch

            # 2) lokal einrasten, Drift linear modellieren
            off = np.arange(-SNAP_PX, SNAP_PX + 0.01, 0.25)
            sc = np.stack([sample(prof, ys + o) for o in off])
            bestoff, strength = off[np.argmax(sc, axis=0)], sc.max(axis=0)
            conf = strength > max(np.percentile(strength, 50), 1e-6)
            if conf.sum() >= 4:
                ys = ys + np.polyval(np.polyfit(ks[conf], bestoff[conf], 1), ks)
            order = np.argsort(ys)
            ys, strength = ys[order] + ey0, strength[order]

            # 3) Randreihen verwerfen
            thr = 0.30 * float(np.percentile(
                cv2.GaussianBlur(mag[by0:by1 + 1, x0:x1 + 1].mean(axis=1).astype(np.float32),
                                 (1, 9), 0), 90))
            e = cv2.GaussianBlur(mag[:, x0:x1 + 1].mean(axis=1).astype(np.float32), (1, 9), 0).ravel()
            keep = []
            for k in range(len(ys) - 1):
                a, b = int(round(ys[k])), int(round(ys[k + 1]))
                if a < 0 or b > H - 1 or b - a < 5:
                    continue
                # Zellmitte muss im Block liegen -> Randreihe bleibt, Fahrgasse nicht
                if not (by0 - 2 <= 0.5 * (a + b) <= by1 + 2):
                    continue
                if float(e[a:b].mean()) > thr:
                    keep.append(k)
            if not keep:
                continue
            lo, hi = keep[0], keep[-1] + 2
            ys, strength = ys[lo:hi], strength[lo:hi]

            # Naht zwischen den beiden Unterspalten
            seg = resp_v[int(ys[0]):int(ys[-1]) + 1, x0:x1 + 1].mean(axis=0)
            c, w = len(seg) // 2, max(6, len(seg) // 6)
            seam = x0 + c - w + int(np.argmax(seg[c - w:c + w + 1]))

            med = max(float(np.median(strength)), 1e-6)
            bl['columns'].append({
                'col': ci,
                'x_range': [int(x0), int(x1)],
                'seam_x': int(seam),
                'row_pitch_px': round(float(pitch), 2),
                'row_lines_y': [round(float(v), 2) for v in ys],
                'line_conf': [round(min(float(s) / med, 1.0), 3) for s in strength],
                'n_rows': len(ys) - 1,
                'n_slots': (len(ys) - 1) * 2,
            })
        grid['blocks'].append(bl)

    grid['total_slots'] = sum(c['n_slots'] for b in grid['blocks'] for c in b['columns'])
    return grid


def summary(grid):
    """Mehrzeilige Zusammenfassung zum Mitlesen."""
    out = []
    for b in grid['blocks']:
        out.append(f"Block {b['block']}  y={b['y_range']}")
        for c in b['columns']:
            ln = c['row_lines_y']
            weak = sum(1 for v in c['line_conf'] if v < 0.4)
            out.append(f"  Spalte {c['col']}: x={c['x_range']} naht={c['seam_x']} "
                       f"teilung={c['row_pitch_px']:.2f} reihen={c['n_rows']:3d} "
                       f"plaetze={c['n_slots']:3d} schwache_linien={weak:2d} "
                       f"y={ln[0]:.0f}..{ln[-1]:.0f}")
        out.append(f"  -> Block gesamt: {sum(c['n_slots'] for c in b['columns'])} Plaetze")
    out.append(f"GESAMT: {grid['total_slots']} Stellplaetze")
    return '\n'.join(out)


def main():
    src = os.path.join(BASE, 'frames', 'frame_0001.jpg')
    img = cv2.imread(src)
    if img is None:
        raise SystemExit(f'Frame nicht gefunden: {src}')
    grid = fit(img)
    grid['source_frame'] = os.path.relpath(src, BASE)
    out = os.path.join(BASE, 'grid.json')
    with open(out, 'w') as f:
        json.dump(grid, f, indent=1)
    print(summary(grid))
    print(f'-> {out}')


if __name__ == '__main__':
    main()
